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
#include "reParams.h"
#include "reLedSys.h"
#include "project_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <time.h>
#include "esp_task_wdt.h"
#include "sys/queue.h"
extern "C" {
#include "esp_mqtt.h"
}
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

minute_timer_callback_t _mintimer_cb = nullptr;

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
    
    // Add a message to the publish queue (three attempts)
    if ((xQueueSend(_mqttQueue, &mqttMsg, CONFIG_MQTT_OUTBOX_QUEUE_WAIT / portTICK_PERIOD_MS) == pdPASS)
     || (xQueueSend(_mqttQueue, &mqttMsg, CONFIG_MQTT_OUTBOX_QUEUE_WAIT / portTICK_PERIOD_MS) == pdPASS)
     || (xQueueSend(_mqttQueue, &mqttMsg, CONFIG_MQTT_OUTBOX_QUEUE_WAIT / portTICK_PERIOD_MS) == pdPASS)) {
      rlog_v(tagMQTTQ, "Message \"%s\" [ %s ] successfully added the queue", topic, payload);
      return true;
    } else {
      rlog_e(tagMQTTQ, "Error adding message to queue [ %s ], topic  [ %s ]!", mqttTaskName, topic);
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
    // esp_task_wdt_reset();
    // taskYIELD();
    vTaskDelay(1);
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
  
  #if CONFIG_MQTT_TIME_AS_PLAIN
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "time"), s_time, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, false);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "date"), s_date, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, false);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "weekday"), s_wday, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, false);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "timeday"), s_timeday, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, false);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "datetime1"), s_datetime1, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, false);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "datetime2"), s_datetime2, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, false);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "year"), malloc_stringf("%d", timeinfo.tm_year+1900), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "month"), malloc_stringf("%d", timeinfo.tm_mon), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "day"), malloc_stringf("%d", timeinfo.tm_mday), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "hour"), malloc_stringf("%d", timeinfo.tm_hour), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "min"), malloc_stringf("%d", timeinfo.tm_min), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "wday"), malloc_stringf("%d", timeinfo.tm_wday), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "yday"), malloc_stringf("%d", timeinfo.tm_yday), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "worktime/days"), malloc_stringf("%d", w_days), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "worktime/hours"), malloc_stringf("%d", w_hours), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
    mqttPublish(mqttGetSubTopic(_mqttTopicTime, "worktime/minutes"), malloc_stringf("%d", w_mins), CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, true, true);
  #endif // CONFIG_MQTT_TIME_AS_PLAIN

  #if CONFIG_MQTT_TIME_AS_JSON
    char * json = malloc_stringf("{\"time\":\"%s\",\"date\":\"%s\",\"weekday\":\"%s\",\"timeday\":\"%s\",\"datetime1\":\"%s\",\"datetime2\":\"%s\",\"year\":%d,\"month\":%d,\"day\":%d,\"hour\":%d,\"min\":%d,\"wday\":%d,\"yday\":%d,\"worktime\":{\"days\":%d,\"hours\":%d,\"minutes\":%d}}", 
      s_time, s_date,  s_wday, s_timeday, s_datetime1, s_datetime2, 
      timeinfo.tm_year+1900, timeinfo.tm_mon, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_wday, timeinfo.tm_yday,
      w_days, w_hours, w_mins);
    if (json) mqttPublish(_mqttTopicTime, json, CONFIG_MQTT_TIME_QOS, CONFIG_MQTT_TIME_RETAINED, true, false, true);
  #endif // CONFIG_MQTT_TIME_AS_JSON
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
    silentModeCheck(timeinfo);
    #endif // CONFIG_SILENT_MODE_ENABLE

    if (_mintimer_cb) _mintimer_cb(timeinfo.tm_mday, timeinfo.tm_wday, timeinfo.tm_hour, timeinfo.tm_min);
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
// ----------------------------------------------------- Utilites --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttIsConnected() 
{
  return (_mqttTask) && esp_mqtt_is_connected();
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
        paramsMqttSubscribesOpen();
        // We send a notification to Telegram
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          if (mqttConnCnt > 1) tgSend(false, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_CONNECT);
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
        break;
      
      case ESP_MQTT_STATUS_DISCONNECTED:
        ledSysStateSet(SYSLED_MQTT_ERROR, false);
        // Resetting the subscriptions of all params
        paramsMqttSubscribesClose();
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
  if ((_mqttTask) && (eTaskGetState(_mqttTask) != eSuspended)) {
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
  if ((_mqttTask) && (eTaskGetState(_mqttTask) == eSuspended)) {
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
  if (!_mqttTask) {
    if (!esp_mqtt_init(mqttOnChangeStatus, paramsMqttIncomingMessage)) {
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
