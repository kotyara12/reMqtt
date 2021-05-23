#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rStrings.h"
#include "rLog.h"
#include "reEsp32.h"
#include "reWifi.h"
#include "reMqtt.h"
#include "reNvs.h"
#include "reLedSys.h"
#include "project_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <time.h>
#include "sys/queue.h"
extern "C" {
#include "esp_mqtt.h"
}
#if CONFIG_MQTT_OTA_ENABLE
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#endif // CONFIG_MQTT_OTA_ENABLE
#if CONFIG_MQTT_SYSINFO_ENABLE
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spi_flash.h"
#endif // CONFIG_MQTT_SYSINFO_ENABLE
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

typedef struct mqttPub_t {
  char* topic;
  char* payload;
  int qos;
  uint8_t remain_cnt;
  bool retained;
  bool forced;
  bool free_topic;
  bool free_payload;
  STAILQ_ENTRY(mqttPub_t) next;
} mqttPub_t;
typedef struct mqttPub_t *mqttPubHandle_t;

STAILQ_HEAD(mqttOutbox_t, mqttPub_t);
typedef struct mqttOutbox_t *mqttOutboxHandle_t;

static const char* tagMQTTQ = "MQTT";
static const char* mqttTaskName = "mqttOutbox";

TaskHandle_t _mqttTask;
QueueHandle_t _mqttQueue = NULL;

#define MQTT_OUTBOX_QUEUE_ITEM_SIZE sizeof(mqttPub_t*)

#if CONFIG_MQTT_STATIC_ALLOCATION
StaticQueue_t _mqttQueueBuffer;
uint8_t _mqttQueueStorage[CONFIG_MQTT_OUTBOX_QUEUE_SIZE * MQTT_OUTBOX_QUEUE_ITEM_SIZE];
StaticTask_t _mqttTaskBuffer;
StackType_t _mqttTaskStack[CONFIG_MQTT_OUTBOX_STACK_SIZE];
#endif // CONFIG_MQTT_STATIC_ALLOCATION

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Publish main -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttPublish(char *topic, char *payload, int qos, bool retained, bool forced, bool free_topic, bool free_payload)
{
  if (_mqttQueue) {
    // Allocating memory for message
    mqttPub_t* mqttMsg = new mqttPub_t;
    mqttMsg->topic = topic;
    mqttMsg->payload = payload;
    mqttMsg->qos = qos;
    mqttMsg->remain_cnt = CONFIG_MQTT_PUBLISH_ATTEMPTS;
    mqttMsg->retained = retained;
    mqttMsg->forced = forced;
    mqttMsg->free_topic = free_topic;
    mqttMsg->free_payload = free_payload;
    STAILQ_NEXT(mqttMsg, next) = NULL;
    
    // Add a message to the publish queue
    if (xQueueSend(_mqttQueue, &mqttMsg, CONFIG_MQTT_OUTBOX_QUEUE_WAIT / portTICK_PERIOD_MS) == pdPASS) {
      rlog_v(tagMQTTQ, "Message \"%s\" [ %s ] successfully added the queue", topic, payload);
      return true;
    } else {
      rlog_e(tagMQTTQ, "Error adding message to queue [ %s ]!", mqttTaskName);
      ledSysStateSet(SYSLED_MQTT_ERROR, false);
      // Removing a message from heap
      if (mqttMsg->free_payload) free(mqttMsg->payload);
      if (mqttMsg->free_topic) free(mqttMsg->topic);
      delete mqttMsg;
    };
  };

  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Publish Outbox ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

mqttOutboxHandle_t mqttOutboxInit()
{
  mqttOutboxHandle_t outbox = new mqttOutbox_t;
  STAILQ_INIT(outbox);
  return outbox;
}

mqttPubHandle_t mqttOutboxFind(mqttOutboxHandle_t outbox, mqttPubHandle_t msg)
{
  mqttPubHandle_t item;
  STAILQ_FOREACH(item, outbox, next) {
    if (strcasecmp(item->topic, msg->topic) == 0) {
      return item;
    }
  }
  return NULL;
} 

bool mqttOutboxPublish(mqttOutboxHandle_t outbox)
{
  mqttPubHandle_t item;
  item = STAILQ_FIRST(outbox);
  if (item) {
    rlog_v(tagMQTTQ, "Processing outbox message \"%s\" [ %d ]", item->topic, strlen(item->payload));
    ledSysOn(true);
    bool send = false;
    if (item->payload) {
      send = esp_mqtt_publish(item->topic, item->payload, item->qos, item->retained);
    } else {
      send = esp_mqtt_publish(item->topic, "", item->qos, item->retained);
    };
    ledSysOff(true);
    if (send) {
      // Removing a message from the send queue
      STAILQ_REMOVE(outbox, item, mqttPub_t, next);
      // Removing a message from heap
      if ((item->payload) && (item->free_payload)) free(item->payload);
      if (item->free_topic) free(item->topic);
      delete item;
      // Resetting the MQTT error status if it was
      ledSysStateClear(SYSLED_MQTT_ERROR, false);
    } else {
      // Failed to send message, set the error flag
      ledSysStateSet(SYSLED_MQTT_ERROR, false);
      // Reducing the number of sending attempts
      item->remain_cnt--;
      if (item->remain_cnt > 0) {
        rlog_e(tagMQTTQ, "Failed send message \"%s\" [ %d ]", item->topic, strlen(item->payload));
      } else {
        rlog_e(tagMQTTQ, "Failed send message \"%s\" [ %d ], message lost", item->topic, strlen(item->payload));
        // Removing a message from the send queue
        STAILQ_REMOVE(outbox, item, mqttPub_t, next);
        // Removing a message from heap
        if (item->free_payload) free(item->payload);
        if (item->free_topic) free(item->topic);
        delete item;
      };
      return false;
    };
  };
  return true;  
}

void mqttOutboxFree(mqttOutboxHandle_t outbox)
{
  mqttPubHandle_t item, tmp;
  STAILQ_FOREACH_SAFE(item, outbox, next, tmp) {
    // Removing a message from the send queue
    STAILQ_REMOVE(outbox, item, mqttPub_t, next);
    // Removing a message from heap
    if (item->free_payload) free(item->payload);
    if (item->free_topic) free(item->topic);
    delete item;
  };
  delete outbox;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------- Publish Date & Time  ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static struct tm timeinfo; 
static uint32_t w_days = 0;
static uint8_t w_hours = 0;
static uint8_t w_mins = -1;
static char s_time[10];
static char s_date[12];
static char s_wday[12];
static char s_timeday[32];
static char s_datetime1[32];
static char s_datetime2[48];

#if CONFIG_MQTT_TIME_ENABLE

static char* _mqttTopicTime = NULL;

void mqttPublishTime()
{
  // rlog_d(tagMQTTQ, "Date and time publishing...");

  char * json = malloc_stringf("{\"time\":\"%s\",\"date\":\"%s\",\"weekday\":\"%s\",\"timeday\":\"%s\",\"datetime1\":\"%s\",\"datetime2\":\"%s\",\"year\":%d,\"month\":%d,\"day\":%d,\"hour\":%d,\"min\":%d,\"wday\":%d,\"yday\":%d,\"worktime\":{\"days\":%d,\"hours\":%d,\"minutes\":%d}}", 
    s_time, s_date,  s_wday, s_timeday, s_datetime1, s_datetime2, 
    timeinfo.tm_year+1900, timeinfo.tm_mon, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_wday, timeinfo.tm_yday,
    w_days, w_hours, w_mins);
  if (json) mqttPublish(_mqttTopicTime, json, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, false, true);
};
#endif // CONFIG_MQTT_TIME_ENABLE

#if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
static char* _mqttTopicStatus = NULL;
#endif // CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE

#if CONFIG_MQTT_SYSINFO_ENABLE
static char* _mqttTopicSysInfo = NULL;
#endif // CONFIG_MQTT_SYSINFO_ENABLE

#if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
void mqttPublishSysInfo()
{
  // rlog_d(tagMQTTQ, "System information publishing...");
  
  double heap_total = (double)heap_caps_get_total_size(MALLOC_CAP_DEFAULT) / 1024.0;
  double heap_free  = (double)heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024.0;
  double heap_min   = (double)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) / 1024.0;
  wifi_ap_record_t wifi_info = wifiInfo();
  esp_netif_ip_info_t wifi_ip = wifiLocalIP();
  uint8_t * ip = (uint8_t*)&(wifi_ip.ip);
  uint8_t * mask = (uint8_t*)&(wifi_ip.netmask);
  uint8_t * gw = (uint8_t*)&(wifi_ip.gw);

  char * s_status = malloc_stringf("%.2d : %.2d : %.2d\nRSSI: %d dBi\n%.1fKb %.0f%%",
    w_days, w_hours, w_mins, wifi_info.rssi, heap_free, 100.0*heap_free/heap_total);
    
  if (s_status) {
    #if CONFIG_MQTT_STATUS_ONLINE
      #if CONFIG_MQTT_STATUS_ONLINE_PAYLOAD_SYSINFO
        mqttPublish(_mqttTopicStatus, malloc_stringf("%s", s_status), 
          CONFIG_MQTT_STATUS_QOS, CONFIG_MQTT_STATUS_RETAINED, true, false, true);
      #else
        mqttPublish(_mqttTopicStatus, (char*)CONFIG_MQTT_STATUS_ONLINE_PAYLOAD, 
          CONFIG_MQTT_STATUS_QOS, CONFIG_MQTT_STATUS_RETAINED, true, false, false);
      #endif // CONFIG_MQTT_STATUS_ONLINE_PAYLOAD_SYSINFO
    #endif // CONFIG_MQTT_STATUS_ONLINE
    
    #if CONFIG_MQTT_SYSINFO_ENABLE
      char * s_wifi = malloc_stringf("{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%d.%d.%d.%d\",\"mask\":\"%d.%d.%d.%d\",\"gw\":\"%d.%d.%d.%d\"}",
        wifi_info.ssid, wifi_info.rssi,
        ip[0], ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3]);
      char * s_work = malloc_stringf("{\"days\":%d,\"hours\":%d,\"minutes\":%d}",
        w_days, w_hours, w_mins);
      char * s_heap = malloc_stringf("{\"total\":\"%.1f\",\"free\":\"%.1f\",\"free_percents\":\"%.0f\",\"free_min\":\"%.1f\",\"free_min_percents\":\"%.0f\"}",
        heap_total, heap_free, 100.0*heap_free/heap_total, heap_min, 100.0*heap_min/heap_total);
      
      if ((s_wifi) && (s_work) && (s_heap) && (s_status)) {
        char * json = malloc_stringf("{\"firmware\":\"%s\",\"wifi\":%s,\"worktime\":%s,\"heap\":%s,\"summary\":\"%s\"}", 
          APP_VERSION, s_wifi, s_work, s_heap, s_status);
        if (json) mqttPublish(_mqttTopicSysInfo, json, 
          CONFIG_MQTT_SYSINFO_QOS, CONFIG_MQTT_SYSINFO_RETAINED, false, false, true);
      };

      if (s_heap) free(s_heap);
      if (s_work) free(s_work);
      if (s_wifi) free(s_wifi);
    #endif // CONFIG_MQTT_SYSINFO_ENABLE

    free(s_status);
  };
};
#endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

#if CONFIG_SILENT_MODE_ENABLE

static uint32_t tsSilentMode = CONFIG_SILENT_MODE_INTERVAL;
static bool stateSilentMode = false;
silent_mode_change_callback_t cbSilentMode = NULL;
static const char* tagSM = "SILENT MODE";

bool silentMode()
{
  return stateSilentMode;
}

void silentModeSetCallback(silent_mode_change_callback_t cb)
{
  cbSilentMode = cb;
}

void silentModeCheck()
{
  if (tsSilentMode > 0) {
    uint16_t t1 = tsSilentMode / 10000;
    uint16_t t2 = tsSilentMode % 10000;
    int16_t  t0 = timeinfo.tm_hour * 100 + timeinfo.tm_min;
    bool newSilentMode = (t1 < t2) ? ((t0 >= t1) && (t0 < t2)) : !((t0 >= t2) && (t1 > t0));
    // If the regime has changed
    if (stateSilentMode != newSilentMode) {
      stateSilentMode = newSilentMode;
      // Switching the system LED (take care of the rest yourself)  
      ledSysSetEnabled(!stateSilentMode);
      // Calling the callback function
      if (cbSilentMode) {
        cbSilentMode(stateSilentMode);
      };
      // Sending alerts
      if (stateSilentMode) {
        rlog_i(tagSM, "Silent mode activated");
        #if CONFIG_SILENT_MODE_TG_NOTIFY
        tgSend(CONFIG_SILENT_MODE_TG_MSG_NOTIFY, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_SILENT_MODE_ON);
        #endif // CONFIG_SILENT_MODE_TG_NOTIFY
      } else {
        rlog_i(tagSM, "Silent mode disabled");
        #if CONFIG_SILENT_MODE_TG_NOTIFY
        tgSend(CONFIG_SILENT_MODE_TG_MSG_NOTIFY, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_SILENT_MODE_OFF);
        #endif // CONFIG_SILENT_MODE_TG_NOTIFY
      };
    };
  };
}

#endif // CONFIG_SILENT_MODE_ENABLE

void mqttFixTime()
{
  static time_t now;
  static uint8_t prev_min = 255;

  // We get the current time
  now = time(NULL);
  localtime_r(&now, &timeinfo);

  // We perform actions only when the minute changes
  if ((timeinfo.tm_year > 70) && (timeinfo.tm_min != prev_min)) {
    prev_min = timeinfo.tm_min;

    // Preparation date and time strings
    strftime(s_time, sizeof(s_time), CONFIG_FORMAT_TIME, &timeinfo);
    strftime(s_date, sizeof(s_date), CONFIG_FORMAT_DATE, &timeinfo);
    switch (timeinfo.tm_wday) {
    case 1:
      strcpy(s_wday, CONFIG_FORMAT_WDAY1);
      break;
    case 2:
      strcpy(s_wday, CONFIG_FORMAT_WDAY2);
      break;
    case 3:
      strcpy(s_wday, CONFIG_FORMAT_WDAY3);
      break;
    case 4:
      strcpy(s_wday, CONFIG_FORMAT_WDAY4);
      break;
    case 5:
      strcpy(s_wday, CONFIG_FORMAT_WDAY5);
      break;
    case 6:
      strcpy(s_wday, CONFIG_FORMAT_WDAY6);
      break;
    case 0:
      strcpy(s_wday, CONFIG_FORMAT_WDAY7);
      break;
    };
    #if CONFIG_FORMAT_WT
      snprintf(s_timeday, sizeof(s_timeday), CONFIG_FORMAT_TIMEDAY, s_wday, s_time);
    #else
      snprintf(s_timeday, sizeof(s_timeday), CONFIG_FORMAT_TIMEDAY, s_time, s_wday);
    #endif // CONFIG_FORMAT_WTD
    #if CONFIG_FORMAT_TD
      snprintf(s_datetime1, sizeof(s_datetime1), CONFIG_FORMAT_DATETIME1, s_time, s_date);
      snprintf(s_datetime2, sizeof(s_datetime2), CONFIG_FORMAT_DATETIME2, s_timeday, s_date);
    #else 
      snprintf(s_datetime1, sizeof(s_datetime1), CONFIG_FORMAT_DATETIME1, s_date, s_time);
      snprintf(s_datetime2, sizeof(s_datetime2), CONFIG_FORMAT_DATETIME2, s_date, s_timeday);
    #endif // CONFIG_FORMAT_TD

    // We count the amount of time worked
    w_mins++;
    if (w_mins > 59) {
      w_mins = 0;
      w_hours++;
      if (w_hours > 23) {
        w_hours = 0;
        w_days++;
      };
    };

    #if CONFIG_MQTT_TIME_ENABLE
    mqttPublishTime();
    #endif // CONFIG_MQTT_TIME_ENABLE

    #if CONFIG_SILENT_MODE_ENABLE
    silentModeCheck();
    #endif // CONFIG_SILENT_MODE_ENABLE
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Subscribe -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttSubscribe(const char *topic, int qos)
{
  bool ret = false;
  if (esp_mqtt_is_connected()) {
    ledSysOn(true);
    ret = esp_mqtt_subscribe(topic, qos);
    ledSysOff(true);
  };
  return ret;
}

bool mqttUnsubscribe(const char *topic)
{
  bool ret = false;
  if (esp_mqtt_is_connected()) {
    ledSysOn(true);
    ret = esp_mqtt_unsubscribe(topic);
    ledSysOff(true);
  };
  return ret;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Outbox Task -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void mqttTaskExec(void *pvParameters)
{
  mqttPubHandle_t mqttMsg;

  // Configuring LWT (status) topic
  #if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
    _mqttTopicStatus = mqttGetTopic1(CONFIG_MQTT_STATUS_TOPIC);
    #if CONFIG_MQTT_STATUS_LWT
    esp_mqtt_set_lwt_topic(_mqttTopicStatus);
    #endif // CONFIG_MQTT_STATUS_LWT
  #endif // CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE

  // Launching the MQTT client
  ledSysOn(true);

  if (!esp_mqtt_start()) {
    rloga_e("Failed to create a MQTT client!");
    #if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
    free(_mqttTopicStatus);
    #endif // CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_CONNECT
    ledSysOff(true);
    ledSysStateSet(SYSLED_ERROR, false);
    mqttTaskDelete();
    return;
  };

  // Creating an outbox
  mqttOutboxHandle_t mqttOutbox = mqttOutboxInit();

  // Periodic (1 time per minute) publication of date and time
  #if CONFIG_MQTT_TIME_ENABLE
    _mqttTopicTime = mqttGetTopic1(CONFIG_MQTT_TIME_TOPIC);
  #endif // CONFIG_MQTT_TIME_ENABLE

  // Topic for system information
  #if CONFIG_MQTT_SYSINFO_ENABLE
    _mqttTopicSysInfo = mqttGetTopic1(CONFIG_MQTT_SYSINFO_TOPIC);
  #endif // CONFIG_MQTT_SYSINFO_ENABLE

  // Timer for posting messages
  #if defined(CONFIG_MQTT_PUBLISH_INTERVAL) && (CONFIG_MQTT_PUBLISH_INTERVAL > 0)
    static esp_lwmqtt_timer_t mqttPubTimer;
    esp_lwmqtt_timer_set(&mqttPubTimer, 0);
  #endif // CONFIG_MQTT_PUBLISH_INTERVAL

  // Timer for posting system information
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    static esp_lwmqtt_timer_t mqttSysInfoTimer;
    esp_lwmqtt_timer_set(&mqttSysInfoTimer, 0);
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

  ledSysOff(true);

  while (true) {
    // If messages are received in the send queue
    if (xQueueReceive(_mqttQueue, &mqttMsg, 1000 / portTICK_PERIOD_MS) == pdPASS) {
      // Let's try to find this topic in the queue
      mqttPubHandle_t findMsg = mqttOutboxFind(mqttOutbox, mqttMsg);
      if (findMsg) {
        // If we find, then we replace its payload and parameters
        if (findMsg->free_payload) free(findMsg->payload);
        findMsg->payload = mqttMsg->payload;
        findMsg->free_payload = mqttMsg->free_payload;
        findMsg->qos = mqttMsg->qos;
        findMsg->retained = mqttMsg->retained;
        // Free memory
        if (mqttMsg->free_topic) free(mqttMsg->topic);
        mqttMsg->payload = NULL; // We passed the payload to another message
        free(mqttMsg);
      }
      else {
        // Otherwise, add an entry to the send queue
        if (mqttMsg->forced) {
          STAILQ_INSERT_HEAD(mqttOutbox, mqttMsg, next);
        }
        else {
          STAILQ_INSERT_TAIL(mqttOutbox, mqttMsg, next);
        };
      };
      rlog_v(tagMQTTQ, "Message \"%s\" [ %d ] successfully added the outbox", mqttMsg->topic, strlen(mqttMsg->payload));
    };

    // Fix and publish current time 
    mqttFixTime();

    // Publish system information
    #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
      if (esp_lwmqtt_timer_get(&mqttSysInfoTimer) < 1) {
        mqttPublishSysInfo();
        esp_lwmqtt_timer_set(&mqttSysInfoTimer, CONFIG_MQTT_SYSINFO_INTERVAL);
      };
    #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

    // Sending first message from the queue
    if (wifiIsConnected() && esp_mqtt_is_connected()) {
      if (!STAILQ_EMPTY(mqttOutbox)) {
        #if defined(CONFIG_MQTT_PUBLISH_INTERVAL) && (CONFIG_MQTT_PUBLISH_INTERVAL > 0)
          if (esp_lwmqtt_timer_get(&mqttPubTimer) < 1) {
            mqttOutboxPublish(mqttOutbox);
            esp_lwmqtt_timer_set(&mqttPubTimer, CONFIG_MQTT_PUBLISH_INTERVAL);
          };
        #else
          mqttOutboxPublish(mqttOutbox);
        #endif // CONFIG_MQTT_PUBLISH_INTERVAL
      };
    };
  };

  // release outbox
  mqttOutboxFree(mqttOutbox);

  // release standard topics
  #if CONFIG_MQTT_TIME_ENABLE
    if (_mqttTopicTime) free(_mqttTopicTime);
  #endif // CONFIG_MQTT_TIME_ENABLE
  #if CONFIG_MQTT_SYSINFO_ENABLE
    if (_mqttTopicSysInfo) free(_mqttTopicSysInfo);
  #endif // CONFIG_MQTT_SYSINFO_ENABLE
  #if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
    if (_mqttTopicStatus) free(_mqttTopicStatus);
  #endif // CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_CONNECT

  // Delete queue and task
  mqttTaskDelete();
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ Options --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

typedef struct paramsEntry_t {
  param_kind_t type_param;
  param_type_t type_value;
  param_change_callback_t on_change;
  const char* friendly;
  const char* group;
  const char* key;
  void *value;
  char *topic;
  #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  char *confirm;
  #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  int qos;
  STAILQ_ENTRY(paramsEntry_t) next;
} paramsEntry_t;
typedef struct paramsEntry_t *paramsEntryHandle_t;

STAILQ_HEAD(paramsHead_t, paramsEntry_t);
typedef struct paramsHead_t *paramsHeadHandle_t;

static paramsHeadHandle_t paramsList = NULL;
static SemaphoreHandle_t paramsLock = NULL;

#define OPTIONS_LOCK() do {} while (xSemaphoreTake(paramsLock, portMAX_DELAY) != pdPASS)
#define OPTIONS_UNLOCK() xSemaphoreGive(paramsLock)

static const char* tagPARAMS = "PARAMS";

bool paramsInit()
{
  nvsInit();

  if (!paramsList) {
    paramsLock = xSemaphoreCreateMutex();
    if (!paramsLock) {
      rlog_e(tagPARAMS, "Can't create parameters mutex!");
      return false;
    };

    paramsList = new paramsHead_t;
    if (paramsList) {
      STAILQ_INIT(paramsList);
    }
    else {
      vSemaphoreDelete(paramsLock);
      rlog_e(tagPARAMS, "Parameters manager initialization error!");
      return false;
    }
  };
  
  #if CONFIG_MQTT_OTA_ENABLE
  paramsRegValue(OPT_KIND_OTA, OPT_TYPE_STRING, nullptr,
    CONFIG_MQTT_SYSTEM_TOPIC, CONFIG_MQTT_OTA_TOPIC, CONFIG_MQTT_OTA_NAME, 
    CONFIG_MQTT_OTA_QOS, NULL);
  #endif // CONFIG_MQTT_OTA_ENABLE

  #if CONFIG_MQTT_COMMAND_ENABLE
  paramsRegValue(OPT_KIND_COMMAND, OPT_TYPE_STRING, nullptr,
    CONFIG_MQTT_SYSTEM_TOPIC, CONFIG_MQTT_COMMAND_TOPIC, CONFIG_MQTT_COMMAND_NAME, 
    CONFIG_MQTT_COMMAND_QOS, NULL);
  #endif // CONFIG_MQTT_COMMAND_ENABLE

  #if CONFIG_SILENT_MODE_ENABLE  
  paramsRegValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr,
    CONFIG_MQTT_COMMON_TOPIC, CONFIG_SILENT_MODE_TOPIC, CONFIG_SILENT_MODE_NAME,
    CONFIG_MQTT_PARAMS_QOS, (void*)&tsSilentMode);
  #endif // CONFIG_SILENT_MODE_ENABLE

  return true;
}

void paramsFree()
{
  OPTIONS_LOCK();

  if (paramsList) {
    paramsEntryHandle_t item, tmp;
    STAILQ_FOREACH_SAFE(item, paramsList, next, tmp) {
      STAILQ_REMOVE(paramsList, item, paramsEntry_t, next);
      if (item->topic) {
        mqttUnsubscribe(item->topic);
        free(item->topic);
      };
      delete item;
    };
    delete paramsList;
  };

  OPTIONS_UNLOCK();
  
  vSemaphoreDelete(paramsLock);
}

void paramsMqttSubscribeEntry(paramsEntryHandle_t entry)
{
  // Generating a topic for a subscription
  char * _topic = nullptr;
  #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  char * _confirm = nullptr;
  #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  if (entry->type_param == OPT_KIND_PARAMETER) {
    _topic = mqttGetTopic(CONFIG_MQTT_PARAMS_TOPIC, entry->group, entry->key);
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    _confirm = mqttGetTopic(CONFIG_MQTT_CONFIRM_TOPIC, entry->group, entry->key);
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  } else {
    _topic = mqttGetTopic(entry->group, entry->key, nullptr);
  };
  if (_topic) {
    rlog_d(tagPARAMS, "Try subscribe to topic: %s", _topic);
    // Trying to subscribe
    if (mqttSubscribe(_topic, entry->qos)) {
      // We succeeded in subscribing, we save the topic to identify incoming messages
      entry->topic = _topic;
      // If confirmation is enabled, publish the current values
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      entry->confirm = _confirm;
      if (entry->confirm) {
        char* str_value = value2string(entry->type_value, entry->value);
        mqttPublish(entry->confirm, str_value, CONFIG_MQTT_CONFIRM_QOS, CONFIG_MQTT_CONFIRM_RETAINED, true, false, false);
        if (str_value) free(str_value);
      };
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    } else {
      // Subscribe failed, delete the topic from heap (it will be generated after reconnecting to the server)
      free(_topic);
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      if (_confirm) free(_confirm);
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    };
  };
}

void paramsMqttSubscribes()
{
  rlog_i(tagPARAMS, "Subscribing to parameter topics...");

  OPTIONS_LOCK();

  if (paramsList) {
    // Recovering subscriptions to topics for which there was no subscription earlier
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      if (item->topic == NULL) {
        paramsMqttSubscribeEntry(item);
        vTaskDelay(CONFIG_MQTT_SUBSCRIBE_INTERVAL / portTICK_RATE_MS);
      };
    };
  };

  OPTIONS_UNLOCK();
}

void paramsMqttResetSubscribes()
{
  rlog_i(tagPARAMS, "Resetting parameter topics...");

  OPTIONS_LOCK();

  if (paramsList) {
    // Delete all topics from the heap
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      free(item->confirm);
      item->confirm = NULL;
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      free(item->topic);
      item->topic = NULL;
    };
  };

  OPTIONS_UNLOCK();
}

void paramsRegValue(const param_kind_t type_param, const param_type_t type_value, param_change_callback_t callback_change,
  const char* name_group, const char* name_key, const char* name_friendly, const int qos, 
  void * value)
{
  if (!paramsList) {
    paramsInit();
  };

  OPTIONS_LOCK();

  if (paramsList) {
    paramsEntryHandle_t item = new paramsEntry_t;
    item->type_param = type_param;
    item->type_value = type_value;
    item->on_change = callback_change;
    item->friendly = name_friendly;
    item->group = name_group;
    item->key = name_key;
    item->topic = NULL;
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    item->confirm = NULL;
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    item->qos = qos;
    item->value = value;
    // Append item to list
    STAILQ_INSERT_TAIL(paramsList, item, next);
    // Read value from NVS storage
    if (item->type_param == OPT_KIND_PARAMETER) {
      nvsRead(item->group, item->key, item->type_value, item->value);
      if (item->on_change) item->on_change();

      char* str_value = value2string(item->type_value, item->value);
      if (item->group) {
        rlog_d(tagPARAMS, "Parameter \"%s.%s\": [%s] registered", item->group, item->key, str_value);
      } else {
        rlog_d(tagPARAMS, "Parameter \"%s\": [%s] registered", item->key, str_value);
      };
      free(str_value);
    } else {
      rlog_d(tagPARAMS, "System handler \"%s\" registered", item->key);
    };
    // We try to subscribe if the connection to the server is already established
    paramsMqttSubscribeEntry(item);
  };
  
  OPTIONS_UNLOCK();
}

#if CONFIG_MQTT_OTA_ENABLE

extern const char ota_pem_start[] asm(CONFIG_MQTT_OTA_PEM_START);
extern const char ota_pem_end[]   asm(CONFIG_MQTT_OTA_PEM_END); 

static const char* tagOTA = "OTA";

void paramsStartOTA(char *topic, uint8_t *payload, size_t len)
{
  if (strlen((char*)payload) > 0) {
    rlog_i(tagOTA, "OTA firmware upgrade received from \"%s\"", (char*)payload);
    #if CONFIG_TELEGRAM_ENABLE
    tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA, (char*)payload);
    #endif // CONFIG_TELEGRAM_ENABLE

    // Resetting the value
    mqttUnsubscribe(topic);
    mqttPublish(topic, nullptr, CONFIG_MQTT_OTA_QOS, CONFIG_MQTT_OTA_RETAINED, true, false, false);
    mqttSubscribe(topic, CONFIG_MQTT_OTA_QOS);

    msTaskDelay(CONFIG_MQTT_OTA_DELAY);

    esp_http_client_config_t cfgOTA;
    memset(&cfgOTA, 0, sizeof(cfgOTA));
    cfgOTA.use_global_ca_store = false;
    cfgOTA.cert_pem = (char*)ota_pem_start;
    cfgOTA.skip_cert_common_name_check = true;
    cfgOTA.url = (char*)payload;
    cfgOTA.is_async = false;

    uint8_t tryUpdate = 0;
    esp_err_t err = ESP_OK;
    ledSysOff(true);
    ledSysStateSet(SYSLED_OTA, true);
    do {
      tryUpdate++;
      rlog_i(tagOTA, "Start of firmware upgrade from \"%s\", attempt %d", (char*)payload, tryUpdate);
      err = esp_https_ota(&cfgOTA);
      if (err == ESP_OK) {
        rlog_i(tagOTA, "Firmware upgrade completed!");
      } else {
        rlog_e(tagOTA, "Firmware upgrade failed: %d!", err);
      };
    } while ((err != ESP_OK) && (tryUpdate < CONFIG_MQTT_OTA_ATTEMPTS));

    #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_OTA_NOTIFY
    if (err == ESP_OK) {
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA_OK, err);
    } else {
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA_FAILED, err);
    };
    #endif // CONFIG_MQTT_OTA_TG_NOTIFY

    msTaskDelay(CONFIG_MQTT_OTA_DELAY);
    rlog_i(tagOTA, "******************* Restart system! *******************");
    esp_restart();
  };
}

#endif // CONFIG_MQTT_OTA_ENABLE

#if CONFIG_MQTT_COMMAND_ENABLE

void paramsExecCmd(char *topic, uint8_t *payload, size_t len)
{
  rlog_i(tagPARAMS, "Command received: [ %s ]", (char*)payload);
  
  #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_COMMAND_NOTIFY
  tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_CMD, (char*)payload);
  #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_COMMAND_NOTIFY

  // Resetting the value
  mqttUnsubscribe(topic);
  mqttPublish(topic, nullptr, CONFIG_MQTT_COMMAND_QOS, CONFIG_MQTT_COMMAND_RETAINED, true, false, false);
  mqttSubscribe(topic, CONFIG_MQTT_COMMAND_QOS);

  // restart controller
  if (strcasecmp((char*)payload, CONFIG_MQTT_CMD_REBOOT) == 0) {
    rlog_i(tagOTA, "******************* Restart system! *******************");
    esp_restart();
  };
}

#endif // CONFIG_MQTT_COMMAND_ENABLE

void paramsSetValue(paramsEntryHandle_t entry, uint8_t *payload, size_t len)
{
  rlog_i(tagPARAMS, "Received parameter [ %s ] from topic \"%s\"", (char*)payload, entry->topic);
  
  // Convert the resulting value to the target format
  void *new_value = string2value(entry->type_value, (char*)payload);
  if (new_value) {
    // If the new value is different from what is already written in the variable...
    if (equal2value(entry->type_value, entry->value, new_value)) {
      rlog_i(tagPARAMS, "Received value does not differ from existing one, ignored");
      // We send a notification to telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_PARAM_EQUAL, 
        entry->friendly, entry->group, entry->key);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    } else {
      // We block context switching to other tasks to prevent reading the value while it is changing
      vTaskSuspendAll();
      // We write the new value to the variable
      setNewValue(entry->type_value, entry->value, new_value);
      if (entry->on_change) entry->on_change();      
      // Restoring the scheduler
      xTaskResumeAll();
      // We save the resulting value in the storage
      nvsWrite(entry->group, entry->key, entry->type_value, entry->value);
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      char* str_value = value2string(entry->type_value, entry->value);
      if (entry->confirm) {
        mqttPublish(entry->confirm, str_value, CONFIG_MQTT_CONFIRM_QOS, CONFIG_MQTT_CONFIRM_RETAINED, true, false, false);
      };
      if (str_value) free(str_value);
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      // We send a notification to telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
      char* tg_value = value2string(entry->type_value, entry->value);
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_PARAM_CHANGE, 
        entry->friendly, entry->group, entry->key, tg_value);
      if (tg_value) free(tg_value);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    };
  } else {
    rlog_e(tagPARAMS, "Could not convert value [ %s ]!", (char*)payload);
    // We send a notification to telegram
    #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_PARAM_BAD, 
      entry->friendly, entry->group, entry->key, (char*)payload);
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
  };
  if (new_value) free(new_value);
}

void mqttOnIncomingMessage(char *topic, uint8_t *payload, size_t len)
{
  OPTIONS_LOCK();
  ledSysOn(true);  

  if (paramsList) {
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      if (strcasecmp(item->topic, topic) == 0) {
        switch (item->type_param) {
          case OPT_KIND_OTA:
            #if CONFIG_MQTT_OTA_ENABLE
            paramsStartOTA(topic, payload, len);
            #endif // CONFIG_MQTT_OTA_ENABLE
            break;
          
          case OPT_KIND_COMMAND:
            #if CONFIG_MQTT_COMMAND_ENABLE
            paramsExecCmd(topic, payload, len);
            #endif // CONFIG_MQTT_COMMAND_ENABLE
            break;

          default:
            paramsSetValue(item, payload, len);
            break;
        };
        ledSysOff(true);
        OPTIONS_UNLOCK();
        return;
      };
    };
  };

  rlog_w(tagPARAMS, "MQTT message from topic [ %s ] was not processed!", topic);
  ledSysOff(true);
  OPTIONS_UNLOCK();
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Status callback -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static uint32_t mqttConnCnt = 0;
static esp_mqtt_status_t prevStatus = ESP_MQTT_STATUS_DISCONNECTED;

void mqttOnChangeStatus(esp_mqtt_status_t status)
{
  if (prevStatus != status) {
    prevStatus = status;
    switch (status) {
      case ESP_MQTT_STATUS_CONNECTED:
        mqttConnCnt++;
        ledSysStateClear(SYSLED_MQTT_ERROR, false);
        // Recovering subscriptions for all params
        paramsMqttSubscribes();
        // We send a notification to Telegram
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          if (mqttConnCnt > 1) tgSend(false, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_CONNECT);
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
        break;
      
      case ESP_MQTT_STATUS_DISCONNECTED:
        ledSysStateSet(SYSLED_MQTT_ERROR, false);
        // Resetting the subscriptions of all params
        paramsMqttResetSubscribes();
        // We send a notification to Telegram
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_DISCONNECT, esp_mqtt_last_error(), esp_mqtt_last_error_str());
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
        break;

      case ESP_MQTT_STATUS_CONNECT_FAILED:
        ledSysStateSet(SYSLED_MQTT_ERROR, false);
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_CONNFAILED, esp_mqtt_last_error(), esp_mqtt_last_error_str());
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
        break;

      case ESP_MQTT_STATUS_TLS_DISABLED:
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_TLSDISABLED);
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
        break;

      default:
        ledSysStateSet(SYSLED_MQTT_ERROR, false);
        break;
    };
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Task routines -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttTaskSuspend()
{
  if ((_mqttTask != NULL) && (eTaskGetState(_mqttTask) != eSuspended)) {
    esp_mqtt_stop();
    vTaskSuspend(_mqttTask);
    rloga_d("Task [ %s ] has been successfully suspended", mqttTaskName);
    return true;
  }
  else {
    rloga_w("Task [ %s ] not found or is already suspended", mqttTaskName);
    return false;
  };
}

bool mqttTaskResume()
{
  if ((_mqttTask != NULL) && (eTaskGetState(_mqttTask) == eSuspended)) {
    vTaskResume(_mqttTask);
    rloga_d("Task [ %s ] has been successfully started", mqttTaskName);
    if (esp_mqtt_start()) {
      return true;
    };
  };
  ledSysStateSet(SYSLED_MQTT_ERROR, false);
  return false;
}

#if CONFIG_MQTT_TLS_ENABLE
bool mqttTaskCreate(bool enable, bool verify, const uint8_t *ca_buf, size_t ca_len) 
#else
bool mqttTaskCreate() 
#endif
{
  if (_mqttTask == NULL) {
    if (!esp_mqtt_init(mqttOnChangeStatus, mqttOnIncomingMessage)) {
      ledSysStateSet(SYSLED_ERROR, false);
      return false;
    };
    #if CONFIG_MQTT_TLS_ENABLE
    if (!esp_mqtt_init_tls(enable, verify, ca_buf, ca_len)) {
      ledSysStateSet(SYSLED_ERROR, false);
      return false;
    };
    #endif

    if (_mqttQueue == NULL) {
      #if CONFIG_MQTT_STATIC_ALLOCATION
      _mqttQueue = xQueueCreateStatic(CONFIG_MQTT_OUTBOX_QUEUE_SIZE, MQTT_OUTBOX_QUEUE_ITEM_SIZE, &(_mqttQueueStorage[0]), &_mqttQueueBuffer);
      #else
      _mqttQueue = xQueueCreate(CONFIG_MQTT_OUTBOX_QUEUE_SIZE, MQTT_OUTBOX_QUEUE_ITEM_SIZE);
      #endif // CONFIG_MQTT_STATIC_ALLOCATION
      if (_mqttQueue == NULL) {
        rloga_e("Failed to create a outbox queue MQTT client!");
        ledSysStateSet(SYSLED_ERROR, false);
        return false;
      };
    };
    
    #if CONFIG_MQTT_STATIC_ALLOCATION
    _mqttTask = xTaskCreateStaticPinnedToCore(mqttTaskExec, mqttTaskName, CONFIG_MQTT_OUTBOX_STACK_SIZE, NULL, CONFIG_MQTT_OUTBOX_PRIORITY, _mqttTaskStack, &_mqttTaskBuffer, CONFIG_MQTT_OUTBOX_CORE); 
    #else
    xTaskCreatePinnedToCore(mqttTaskExec, mqttTaskName, CONFIG_MQTT_OUTBOX_STACK_SIZE, NULL, CONFIG_MQTT_OUTBOX_PRIORITY, &_mqttTask, CONFIG_MQTT_OUTBOX_CORE); 
    #endif // CONFIG_MQTT_STATIC_ALLOCATION
    if (_mqttTask == NULL) {
      vQueueDelete(_mqttQueue);
      rloga_e("Failed to create task for sending data to MQTT!");
    }
    else {
      rloga_d("Task [ %s ] has been successfully started", mqttTaskName);
      return true;
    };

    ledSysStateSet(SYSLED_ERROR, false);
    return false;
  }
  else {
    return mqttTaskResume();
  };
}

bool mqttTaskDelete()
{
  esp_mqtt_stop();

  if (_mqttQueue != NULL) {
    vQueueDelete(_mqttQueue);
    rloga_v("Pubs outbox queue MQTT client has been deleted");
    _mqttQueue = NULL;
  };

  if (_mqttTask != NULL) {
    vTaskDelete(_mqttTask);
    _mqttTask = NULL;
    rloga_d("Task [ %s ] was deleted", mqttTaskName);
  };
  
  esp_mqtt_free();

  return true;
}
