# MQTT client for ESP32 / ESP-IDF

MQTT client for ESP32 (ESP-IDF) with outbound send queue (for servers with call intervals), based on esp-mqtt by Joël Gähwiler (https://github.com/256dpi/esp-mqtt)

## Dependencies:
  - https://github.com/kotyara12/rLog
  - https://github.com/kotyara12/rStrings
  - https://github.com/kotyara12/reEsp32
  - https://github.com/kotyara12/reLed
  - https://github.com/kotyara12/reWifi
  - https://github.com/kotyara12/reNvs
  - https://github.com/kotyara12/reTgSend (optional)

### Notes:
  - libraries starting with the <b>re</b> prefix are only suitable for ESP32 and ESP-IDF
  - libraries starting with the <b>ra</b> prefix are only suitable for ARDUINO compatible code
  - libraries starting with the <b>r</b> prefix can be used in both cases (in ESP-IDF and in ARDUINO)