/* 
   EN: MQTT client for ESP32 (ESP-IDF) with outbound send queue (for servers with call intervals)
   RU: Клиент MQTT ESP32 (ESP-IDF) с очередью отправки исходящих сообщений (для серверов с интервалами обращения)
   --------------------------
   (с) 2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_MQTT_H__
#define __RE_MQTT_H__

#include <stddef.h>
#include <stdbool.h>
#include "project_config.h"
#include "rTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_lwmqtt.h"
#include "esp_mqtt.h"

#if CONFIG_SILENT_MODE_ENABLE
typedef void (*silent_mode_change_callback_t) (const bool silent_mode);
#endif // CONFIG_SILENT_MODE_ENABLE

#if CONFIG_MQTT_TLS_ENABLE
bool mqttTaskCreate(bool enable, bool verify, const uint8_t *ca_buf, size_t ca_len);
#else
bool mqttTaskCreate();
#endif // CONFIG_MQTT_TLS_ENABLE
bool mqttTaskSuspend();
bool mqttTaskResume();
bool mqttTaskDelete();

bool mqttSubscribe(const char *topic, int qos);
bool mqttUnsubscribe(const char *topic);
bool mqttPublish(char *topic, char *payload, int qos, bool retained, bool forced, bool free_topic, bool free_payload);

#if CONFIG_SILENT_MODE_ENABLE
bool silentMode();
void silentModeSetCallback(silent_mode_change_callback_t cb);
#endif // CONFIG_SILENT_MODE_ENABLE

#ifdef __cplusplus
}
#endif

#endif // __RE_MQTT_H__

