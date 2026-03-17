# ESP32 Bluetooth MP3 Player

Streams MP3 audio from SD card to a Bluetooth A2DP speaker using ESP32.

## Features
- Downloads MP3 over WiFi (one time) and saves to SD card
- Decodes MP3 using libmad (fixed-point)
- Streams decoded PCM audio over Bluetooth A2DP to any speaker
- Tested with Saregama Carvaan

## Hardware
- ESP32
- SD card (SPI: MOSI=22, MISO=21, CLK=26, CS=16)

## Build
Built with ESP-IDF v5.5.2
