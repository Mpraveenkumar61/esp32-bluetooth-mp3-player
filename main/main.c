#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

static esp_bd_addr_t peer_bd_addr = {0};

/* ========== AUDIO CALLBACK (STEREO 44.1kHz) ========== */
#include "esp_spiffs.h"

FILE *wav_file = NULL;

int32_t audio_data_callback(uint8_t *data, int32_t len)
{
    if (!wav_file) return 0;

    size_t read_bytes = fread(data, 1, len, wav_file);

    if (read_bytes < len) {
        fseek(wav_file, 44, SEEK_SET);  // skip WAV header
        read_bytes += fread(data + read_bytes, 1, len - read_bytes, wav_file);
    }

    return read_bytes;
}
/* ========== A2DP CALLBACK ========== */
void a2dp_cb(esp_a2d_cb_event_t event,
             esp_a2d_cb_param_t *param)
{
    switch (event) {

    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            printf("A2DP Connected!\n");

            // START STREAMING
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        }
        break;

    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        printf("Media Start Acknowledged\n");
        break;

    default:
        break;
    }
}

/* ========== GAP CALLBACK ========== */
static esp_bd_addr_t saved_bd_addr;
static bool device_found = false;

void my_gap_callback(esp_bt_gap_cb_event_t event,
                     esp_bt_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_BT_GAP_DISC_RES_EVT:

        for (int i = 0; i < param->disc_res.num_prop; i++) {

            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR) {

                uint8_t *eir = param->disc_res.prop[i].val;
                uint8_t len;
                uint8_t *name = esp_bt_gap_resolve_eir_data(
                        eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);

                if (name) {
                    if (strstr((char *)name, "Carvaan")) {

                        printf("Carvaan Found!\n");

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

            if (device_found) {
                printf("Connecting to Carvaan...\n");
                esp_a2d_source_connect(saved_bd_addr);
            }
        }
        break;

    default:
        break;
    }
}

/* ========== MAIN ========== */
void app_main(void)
{
    nvs_flash_init();
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = NULL,
    .max_files = 5,
    .format_if_mount_failed = true
};

esp_vfs_spiffs_register(&conf);

wav_file = fopen("/spiffs/music.wav", "rb");
fseek(wav_file, 44, SEEK_SET); // skip header
    esp_bt_controller_config_t bt_cfg =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);

    esp_bluedroid_init();
    esp_bluedroid_enable();

    esp_bt_gap_register_callback(my_gap_callback);

    esp_a2d_register_callback(a2dp_cb);
    esp_a2d_source_register_data_callback(audio_data_callback);
    esp_a2d_source_init();

    esp_bt_gap_set_device_name("ESP32_A2DP_SRC");

    esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE,
        ESP_BT_GENERAL_DISCOVERABLE);

    printf("Starting device discovery...\n");

    esp_bt_gap_start_discovery(
        ESP_BT_INQ_MODE_GENERAL_INQUIRY,
        10,
        0);
}