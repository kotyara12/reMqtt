/* 
   EN: MQTT client for ESP32 (ESP-IDF), delayed message send queue
   RU: MQTT клиент для ESP32 (ESP-IDF), очередь отправки сообщений с задержкой
   --------------------------
   (с) 2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RMQTT32_H__
#define __RMQTT32_H__

#include <stddef.h>
#include <stdbool.h>
#include "project_config.h"
#include "rTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_lwmqtt.h"
#include "esp_mqtt.h"

typedef void (*param_change_callback_t) (); 
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

bool paramsInit();
void paramsFree();
void paramsRegValue(const param_kind_t type_param, const param_type_t type_value, param_change_callback_t callback_change,
  const char* name_group, const char* name_key, const char* name_friendly, const int qos, 
  void * value);

#if CONFIG_SILENT_MODE_ENABLE
bool silentMode();
void silentModeSetCallback(silent_mode_change_callback_t cb);
#endif // CONFIG_SILENT_MODE_ENABLE

#ifdef __cplusplus
}
#endif

#endif // __RMQTT32_H__

