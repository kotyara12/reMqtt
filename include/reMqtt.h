/* 
   EN: MQTT client for ESP32 (ESP-IDF) with outbound send queue (for servers with call intervals)
   RU: Клиент MQTT ESP32 (ESP-IDF) с очередью отправки исходящих сообщений (для серверов с интервалами обращения)
   --------------------------
   (с) 2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_MQTT_H__
#define __RE_MQTT_H__

#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event_base.h"
#include "project_config.h"
#include "def_consts.h"
#include "rLog.h"
#include "rTypes.h"
#include "rStrings.h"
#include "reStates.h"
#include "reEvents.h"
#include "reEsp32.h"
#include "reWifi.h"
#include "reNvs.h"
#if CONFIG_MQTT_USE_LWMQTT_CLIENT
  #include "lwmqtt.h" 
#else
  #include "mqtt_client.h"
#endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
char* mqttTopicStatusCreate(const bool primary);
char* mqttTopicStatusGet();
void  mqttTopicStatusFree();
#endif // CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE

bool mqttTaskStart(bool createSuspended);
bool mqttTaskRestart();
bool mqttTaskFree();

bool mqttEventHandlerRegister();
void mqttEventHandlerUnregister();

bool mqttIsConnected();
int  mqttGetOutboxSize();
bool mqttSubscribe(const char *topic, int qos);
bool mqttUnsubscribe(const char *topic);
esp_err_t mqttPublish(char *topic, char *payload, int qos, bool retained, bool free_topic, bool free_payload);

#ifdef __cplusplus
}
#endif

#endif // __RE_MQTT_H__

