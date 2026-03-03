# ESP32 A2DP Source • Carvaan Mini 🎧

An **ESP32 Classic Bluetooth A2DP Source** demo that streams audio (e.g., sine tone and future WAV files) from an ESP32 to a Bluetooth speaker like the **Saregama Carvaan Mini** using **ESP-IDF v5.x**.

---

## 🚀 Features

✅ Classic Bluetooth A2DP Source implementation  
✅ Auto discovery & connect to Carvaan Mini  
✅ Generates real-time 44.1kHz stereo PCM audio  
✅ ESP-IDF native solution — no Arduino  
✅ Simple audio callback for custom sound

---

## 🛠 Supported Audio Format

If you load audio data from a file (future feature):

| Property | Value |
|----------|--------|
| Format | `.wav` |
| Codec | PCM |
| Bitrate | 1411 kbps |
| Sample rate | 44.1 kHz |
| Channels | Stereo |

> NOTE: This is the native SBC/A2DP expected format for Bluetooth audio streaming. (Source: ESP32-A2DP docs)

---

## 📦 Folder Structure

```sh
.
├── main
│   └── main.c                 # Your A2DP Source code
├── components
│   └── ESP32-A2DP            # (optional future WAV/decoder libs)
├── CMakeLists.txt
├── sdkconfig                # Excluded from repo via .gitignore
├── .gitignore
└── README.md
Build & Flash

Ensure you have the ESP-IDF environment set up:

cd ESP32-A2DP-Source-Carvaan
idf.py fullclean
idf.py build
idf.py -p COM3 flash monitor

Replace COM3 with your serial port.

📡 Pair & Play

Turn ON your Carvaan Mini

Switch to Bluetooth mode

ESP32 will start discovery automatically


Pair & connect — simple audio should start

📈 Expected Output (Serial)
Starting device discovery...
Found device: Carvaan mini
Carvaan Found! Connecting...
A2DP Connected!
Media Start Acknowledged
❗ Common Notes

BT_BTC: A2DP Enable without AVRC: This warning is OK

osi_mem_dbg_... warnings are harmless for audio

Classic Bluetooth only — BLE memory is released

🚧 Next Enhancements

✔ Play WAV from SPIFFS
✔ Add MP3 decoding support
✔ Stream audio over Wi-Fi
✔ Add control buttons (play/pause/next)
