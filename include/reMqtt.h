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
#include "mqtt_client.h"
#include "rTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mqttTaskCreate();
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

