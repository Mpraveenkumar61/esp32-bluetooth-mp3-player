#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "mad.h"

/* ========== PIN DEFINITIONS ========== */
#define SD_PIN_MISO     21
#define SD_PIN_MOSI     22
#define SD_PIN_CLK      26
#define SD_PIN_CS       16

/* ========== WIFI CREDENTIALS ========== */
#define WIFI_SSID       "iPhone"
#define WIFI_PASSWORD   "mpraveenn"

/* ========== SERVER URL ========== */
#define MP3_URL         "http://172.20.10.3:8080/MUSIC.MP3"
#define MP3_PATH        "/sdcard/MUSIC.MP3"

/* ========== NVS KEYS ========== */
#define NVS_NAMESPACE   "storage"
#define NVS_KEY         "mp3_ready"

/* ========== RING BUFFER ========== */
#define PCM_RING_BUF_SIZE    (8 * 1024)
#define PCM_PREBUFFER_BYTES  (4 * 1024)

static RingbufHandle_t pcm_ring_buf = NULL;

/* ========== GLOBALS ========== */
static sdmmc_card_t      *sd_card           = NULL;
static EventGroupHandle_t wifi_event_group;
static bool               device_found      = false;
static esp_bd_addr_t      saved_bd_addr;
static volatile bool      bt_streaming      = false;
static FILE              *mp3_file_global   = NULL;
static bool               decoder_started   = false;
static TaskHandle_t       decoder_task_handle = NULL;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      10
static int wifi_retry_count = 0;

/* ========== NVS HELPERS ========== */
static bool is_mp3_ready(void)
{
    nvs_handle_t handle;
    uint8_t val = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, NVS_KEY, &val);
        nvs_close(handle);
    }
    return val == 1;
}

static void set_mp3_ready(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY, 1);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void clear_mp3_ready(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

/* ========== SD CARD INIT ========== */
static esp_err_t sd_card_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 4000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 20000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        printf("SPI bus init failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config,
                                   &mount_config, &sd_card);
    if (ret != ESP_OK) {
        printf("SD mount failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    printf("SD card mounted successfully\n");
    sdmmc_card_print_info(stdout, sd_card);
    return ESP_OK;
}

/* ========== WIFI ========== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        printf("WiFi connected! IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d =
            (wifi_event_sta_disconnected_t *)event_data;
        printf("WiFi disconnected, reason: %d\n", d->reason);
        if (wifi_retry_count < WIFI_MAX_RETRY) {
            wifi_retry_count++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid               = WIFI_SSID,
            .password           = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    printf("Waiting for WiFi...\n");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        false, false, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        printf("WiFi ready!\n");
    } else {
        printf("WiFi FAILED! Check credentials.\n");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
}

static void wifi_stop(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();
    printf("WiFi stopped\n");
}

/* ========== HTTP DOWNLOAD ========== */
static bool download_file(const char *url, const char *save_path)
{
    printf("Downloading: %s\n", url);

    FILE *f = fopen(save_path, "wb");
    if (!f) {
        printf("Cannot open file for writing\n");
        return false;
    }

    esp_http_client_config_t config = {
        .url         = url,
        .timeout_ms  = 30000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        printf("HTTP open failed: %s\n", esp_err_to_name(err));
        fclose(f);
        remove(save_path);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    printf("File size: %d bytes\n", content_length);

    char buffer[4096];
    int  total      = 0;
    int  read_len   = 0;
    int  last_print = 0;

    while ((read_len = esp_http_client_read(
                client, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, read_len, f);
        total += read_len;
        if (total - last_print >= 102400) {
            printf("Downloaded: %d / %d KB\n",
                   total / 1024, content_length / 1024);
            last_print = total;
        }
    }

    fclose(f);
    esp_http_client_cleanup(client);

    if (total < 1024) {
        printf("Download too small: %d bytes\n", total);
        remove(save_path);
        return false;
    }

    printf("Download complete! %d bytes\n", total);
    return true;
}

/* ========== LIBMAD DECODER ========== */
#define MP3_READ_BUF_SIZE   2048

static inline int16_t mad_fixed_to_s16(mad_fixed_t sample)
{
    if (sample >= MAD_F_ONE)  return 32767;
    if (sample <= -MAD_F_ONE) return -32768;
    return (int16_t)(sample >> (MAD_F_FRACBITS - 15));
}

static void mp3_decoder_task(void *pvParameters)
{
    (void)pvParameters;

    printf("Decoder task started, free heap: %lu\n",
           (unsigned long)esp_get_free_heap_size());

    struct mad_stream *stream   = malloc(sizeof(struct mad_stream));
    struct mad_frame  *frame    = malloc(sizeof(struct mad_frame));
    struct mad_synth  *synth    = malloc(sizeof(struct mad_synth));
    int16_t           *pcm_buf  = malloc(1152 * 2 * sizeof(int16_t));
    uint8_t           *input_buf = malloc(MP3_READ_BUF_SIZE);

    if (!stream || !frame || !synth || !pcm_buf || !input_buf) {
        printf("DECODER MALLOC FAILED! stream=%p frame=%p synth=%p pcm=%p in=%p\n",
               stream, frame, synth, pcm_buf, input_buf);
        printf("Free heap after fail: %lu\n", (unsigned long)esp_get_free_heap_size());
        free(stream); free(frame); free(synth);
        free(pcm_buf); free(input_buf);
        vTaskDelete(NULL);
        return;
    }

    if (!mp3_file_global) {
        printf("mp3_file_global is NULL! Aborting decoder.\n");
        free(stream); free(frame); free(synth);
        free(pcm_buf); free(input_buf);
        vTaskDelete(NULL);
        return;
    }

    printf("Decoder mallocs OK. Starting decode loop...\n");

    mad_stream_init(stream);
    mad_frame_init(frame);
    mad_synth_init(synth);

    size_t bytes_remaining = 0;

    while (1) {

        if (stream->buffer == NULL || stream->error == MAD_ERROR_BUFLEN) {

            size_t read_size = MP3_READ_BUF_SIZE - bytes_remaining;

            if (bytes_remaining > 0) {
                memmove(input_buf, stream->next_frame, bytes_remaining);
            }

            size_t bytes_read = fread(input_buf + bytes_remaining,
                                      1, read_size, mp3_file_global);

            if (bytes_read == 0) {
                if (feof(mp3_file_global)) {
                    printf("EOF — looping\n");
                    fseek(mp3_file_global, 0, SEEK_SET);
                    bytes_remaining = 0;
                    mad_stream_finish(stream);
                    mad_stream_init(stream);
                    continue;
                }
                printf("fread error, stopping decoder\n");
                break;
            }

            bytes_remaining += bytes_read;
            mad_stream_buffer(stream, input_buf, bytes_remaining);
            stream->error = MAD_ERROR_NONE;
        }

        if (mad_frame_decode(frame, stream) == -1) {
            if (MAD_RECOVERABLE(stream->error)) {
                continue;
            } else if (stream->error == MAD_ERROR_BUFLEN) {
                bytes_remaining = stream->bufend - stream->next_frame;
                continue;
            } else {
                printf("Unrecoverable MAD error: 0x%04x\n", stream->error);
                break;
            }
        }

        mad_synth_frame(synth, frame);

        int num_samples = synth->pcm.length;
        int out_idx     = 0;

        for (int i = 0; i < num_samples; i++) {
            pcm_buf[out_idx++] = mad_fixed_to_s16(synth->pcm.samples[0][i]);
            if (MAD_NCHANNELS(&frame->header) == 2) {
                pcm_buf[out_idx++] = mad_fixed_to_s16(synth->pcm.samples[1][i]);
            } else {
                pcm_buf[out_idx++] = mad_fixed_to_s16(synth->pcm.samples[0][i]);
            }
        }

        size_t pcm_bytes = out_idx * sizeof(int16_t);

        BaseType_t sent = pdFALSE;
        while (sent != pdTRUE) {
            sent = xRingbufferSend(pcm_ring_buf, pcm_buf,
                                   pcm_bytes, pdMS_TO_TICKS(100));
            if (sent != pdTRUE) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        bytes_remaining = stream->bufend - stream->next_frame;
    }

    mad_synth_finish(synth);
    mad_frame_finish(frame);
    mad_stream_finish(stream);
    free(synth);
    free(frame);
    free(stream);
    free(pcm_buf);
    free(input_buf);
    printf("Decoder task exiting\n");
    vTaskDelete(NULL);
}

/* ========== AUDIO CALLBACK ========== */
int32_t audio_data_callback(uint8_t *data, int32_t len)
{
    if (!pcm_ring_buf) {
        memset(data, 0, len);
        return len;
    }

    int32_t total_read = 0;

    while (total_read < len) {
        size_t  item_size = 0;
        void   *item = xRingbufferReceiveUpTo(
                            pcm_ring_buf,
                            &item_size,
                            pdMS_TO_TICKS(20),
                            len - total_read);
        if (item == NULL) break;

        memcpy(data + total_read, item, item_size);
        vRingbufferReturnItem(pcm_ring_buf, item);
        total_read += item_size;
    }

    if (total_read < len) {
        memset(data + total_read, 0, len - total_read);
    }

    return len;
}

/* ========== PRE-BUFFER TASK ========== */
static void prebuffer_then_start_task(void *pvParameters)
{
    (void)pvParameters;
    printf("Waiting for ring buffer to fill (%d bytes)...\n",
           PCM_PREBUFFER_BYTES);

    while (1) {
        UBaseType_t waiting = 0;
        vRingbufferGetInfo(pcm_ring_buf, NULL, NULL, NULL, NULL, &waiting);

        if ((int)waiting >= PCM_PREBUFFER_BYTES) {
            printf("Pre-buffer ready (%u bytes)! Sending A2DP START...\n",
                   (unsigned)waiting);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    vTaskDelete(NULL);
}
/* ========== SKIP ID3 TAG ========== */
static void skip_id3_tag(FILE *f)
{
    uint8_t header[10];
    if (fread(header, 1, 10, f) != 10) {
        fseek(f, 0, SEEK_SET);
        return;
    }

    // Check for "ID3" magic
    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        // ID3v2 size is encoded as 4 x 7-bit bytes (synchsafe integer)
        uint32_t size = ((uint32_t)(header[6] & 0x7F) << 21) |
                        ((uint32_t)(header[7] & 0x7F) << 14) |
                        ((uint32_t)(header[8] & 0x7F) <<  7) |
                        ((uint32_t)(header[9] & 0x7F));
        size += 10; // add header itself

        // Check for extended header flag (bit 6 of flags byte)
        if (header[5] & 0x10) size += 10; // footer present

        printf("ID3 tag found, skipping %lu bytes\n", (unsigned long)size);
        fseek(f, size, SEEK_SET);
    } else {
        // No ID3 tag, rewind
        fseek(f, 0, SEEK_SET);
    }
}

/* ========== A2DP CALLBACK ========== */
void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {

    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            printf("A2DP Connected! Starting decoder and pre-buffer...\n");

            if (!decoder_started) {
                decoder_started = true;
                xTaskCreate(mp3_decoder_task,
                            "mp3_dec",
                            12288,
                            NULL,
                            5,
                            &decoder_task_handle);
            }

            xTaskCreate(prebuffer_then_start_task,
                        "prebuf",
                        1536,
                        NULL,
                        4,
                        NULL);

        } else if (param->conn_stat.state ==
                   ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            printf("A2DP Disconnected, restarting discovery...\n");
            bt_streaming    = false;
            device_found    = false;
            decoder_started = false;
            esp_bt_gap_start_discovery(
                ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        printf("Codec config received (codec type: %d)\n",
               param->audio_cfg.mcc.type);
        break;

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd  == ESP_A2D_MEDIA_CTRL_START &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            printf("Media Start Acknowledged — streaming!\n");
            bt_streaming = true;
        }
        break;

    default:
        break;
    }
}

/* ========== GAP CALLBACK ========== */
void my_gap_callback(esp_bt_gap_cb_event_t event,
                     esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR) {
                uint8_t *eir = param->disc_res.prop[i].val;
                uint8_t  name_len;
                uint8_t *name = esp_bt_gap_resolve_eir_data(
                        eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
                if (!name) {
                    name = esp_bt_gap_resolve_eir_data(
                        eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
                }
                if (name) {
                    char name_str[64] = {0};
                    int copy_len = name_len < 63 ? name_len : 63;
                    memcpy(name_str, name, copy_len);
                    printf("Found device: %s\n", name_str);
                    if (strstr(name_str, "Carvaan")) {
                        printf("Carvaan found! Connecting...\n");
                        memcpy(saved_bd_addr,
                               param->disc_res.bda,
                               ESP_BD_ADDR_LEN);
                        device_found = true;
                        esp_bt_gap_cancel_discovery();
                    }
                }
            }
        }
        break;

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            printf("Discovery stopped\n");
            if (bt_streaming) {
                printf("Already streaming, skip rediscovery\n");
                break;
            }
            if (device_found) {
                printf("Connecting to Carvaan...\n");
                esp_a2d_source_connect(saved_bd_addr);
            } else {
                printf("Not found, retrying in 2s...\n");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_bt_gap_start_discovery(
                    ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        }
        break;

    default:
        break;
    }
}

/* ========== BLUETOOTH INIT ========== */
static void bt_init(void)
{
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);

    esp_bluedroid_init();
    esp_bluedroid_enable();

    esp_bt_gap_register_callback(my_gap_callback);
    esp_a2d_register_callback(a2dp_cb);
    esp_a2d_source_register_data_callback(audio_data_callback);
    esp_a2d_source_init();

    esp_bt_gap_set_device_name("ESP32_A2DP_SRC");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

/* ========== MAIN ========== */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (sd_card_init() != ESP_OK) {
        printf("SD init failed.\n");
        return;
    }

    if (!is_mp3_ready()) {
        printf("\n===== PHASE 1: DOWNLOAD =====\n");
        printf("Server: %s\n", MP3_URL);

        wifi_init();

        bool ok = download_file(MP3_URL, MP3_PATH);
        if (!ok) {
            printf("Download failed! Check server and retry.\n");
            wifi_stop();
            while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
        }

        struct stat st;
        if (stat(MP3_PATH, &st) == 0)
            printf("Verified: %ld bytes on SD\n", st.st_size);

        wifi_stop();
        set_mp3_ready();
        printf("Rebooting into playback mode...\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    printf("\n===== PHASE 2: PLAYBACK =====\n");

    struct stat st;
    if (stat(MP3_PATH, &st) != 0 || st.st_size < 1024) {
        printf("MP3 missing or corrupt, re-downloading...\n");
        clear_mp3_ready();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    printf("MP3 file: %ld bytes (%.1f MB)\n",
           st.st_size, st.st_size / (1024.0f * 1024.0f));
    printf("Free heap before BT: %lu\n", (unsigned long)esp_get_free_heap_size());

    mp3_file_global = fopen(MP3_PATH, "rb");
    if (!mp3_file_global) {
        printf("Failed to open MP3!\n");
        return;
    }
    skip_id3_tag(mp3_file_global);
    printf("MP3 opened successfully\n");

    pcm_ring_buf = xRingbufferCreate(PCM_RING_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!pcm_ring_buf) {
        printf("Failed to create ring buffer!\n");
        return;
    }
    printf("Ring buffer created (%d bytes)\n", PCM_RING_BUF_SIZE);

    printf("Starting Bluetooth...\n");
    bt_init();

    printf("Starting device discovery...\n");
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}