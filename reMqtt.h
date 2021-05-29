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

typedef void (*minute_timer_callback_t) (const uint8_t mday, const uint8_t wday, const uint8_t hour, const uint8_t min); 

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_lwmqtt.h"
#include "esp_mqtt.h"

#if CONFIG_MQTT_TLS_ENABLE
bool mqttTaskCreate(bool enable, bool verify, const uint8_t *ca_buf, size_t ca_len);
#else
bool mqttTaskCreate();
#endif // CONFIG_MQTT_TLS_ENABLE
bool mqttTaskSuspend();
bool mqttTaskResume();
bool mqttTaskDelete();

bool mqttIsConnected();
bool mqttSubscribe(const char *topic, int qos);
bool mqttUnsubscribe(const char *topic);
bool mqttPublish(char *topic, char *payload, int qos, bool retained, bool forced, bool free_topic, bool free_payload);

#ifdef __cplusplus
}
#endif

#endif // __RE_MQTT_H__

