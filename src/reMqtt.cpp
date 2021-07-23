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
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

static const char* tagMQTT = "MQTT";

#define MQTT_LOG_PAYLOAD_LIMIT 2048

#if CONFIG_MQTT_TLS_ENABLE
extern const uint8_t mqtt_broker_pem_start[] asm(CONFIG_MQTT_TLS_PEM_START);
extern const uint8_t mqtt_broker_pem_end[]   asm(CONFIG_MQTT_TLS_PEM_END); 
#endif // CONFIG_MQTT_TLS_ENABLE

static esp_mqtt_client_handle_t _mqttClient = nullptr;
static bool _mqttConnected = false;
static uint32_t _mqttConnCnt = 0;

#ifdef CONFIG_MQTT_STATUS_LWT
static char* _mqttStatusTopic = nullptr;
#endif // CONFIG_MQTT_STATUS_LWT

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Subscribe -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttSubscribe(const char *topic, int qos)
{
  if (_mqttConnected) {
    ledSysOn(true);
    int msq_id = esp_mqtt_client_subscribe(_mqttClient, topic, qos);
    ledSysOff(true);
    if (msq_id == -1) {
      rlog_e(tagMQTT, "Failed to subscribe to topic [%s]!", topic);
      ledSysStateSet(SYSLED_MQTT_ERROR, false);
      return false;
    };
    return true;
  };
  rlog_e(tagMQTT, "Client is not connected!");
  ledSysStateSet(SYSLED_MQTT_ERROR, false);
  return false;
}

bool mqttUnsubscribe(const char *topic)
{
  if (_mqttConnected) {
    ledSysOn(true);
    int msq_id = esp_mqtt_client_unsubscribe(_mqttClient, topic);
    ledSysOff(true);
    if (msq_id == -1) {
      rlog_e(tagMQTT, "Failed to unsubscribe from topic [%s]!", topic);
      ledSysStateSet(SYSLED_MQTT_ERROR, false);
      return false;
    };
    return true;
  };
  rlog_e(tagMQTT, "Client is not connected!");
  ledSysStateSet(SYSLED_MQTT_ERROR, false);
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Utilites --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttIsConnected() 
{
  return wifiIsConnected() && _mqttConnected;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ Publish --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttPublish(char *topic, char *payload, int qos, bool retained, bool forced, bool free_topic, bool free_payload)
{
  if (_mqttConnected) {
    return true;
  };
  rlog_e(tagMQTT, "Client is not connected!");
  ledSysStateSet(SYSLED_MQTT_ERROR, false);
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Publish Date & Time -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

/*
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
*/

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Outbox Task -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

/*
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
    rlog_e("Failed to create a MQTT client!");
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
*/

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Event callback -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static esp_err_t mqttEventHandlerCb(esp_mqtt_event_handle_t event)
{
  // esp_mqtt_client_handle_t client = event->client;
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      _mqttConnected = true;
      _mqttConnCnt++;
      ledSysStateClear(SYSLED_MQTT_ERROR, false);
      rlog_i(tagMQTT, "Connection to MQTT server established");
      // Recovering subscriptions for all params
      paramsMqttSubscribesOpen();
      // We send a notification to Telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
        if (_mqttConnCnt > 1) tgSend(false, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_CONNECT);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
      break;
    case MQTT_EVENT_DISCONNECTED:
      _mqttConnected = false;
      ledSysStateSet(SYSLED_MQTT_ERROR, false);
      rlog_w(tagMQTT, "Lost connection to MQTT server");
      // Resetting the subscriptions of all params
      paramsMqttSubscribesClose();
      // We send a notification to Telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
        tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_DISCONNECT);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
      break;

    case MQTT_EVENT_SUBSCRIBED:
      ledSysStateClear(SYSLED_MQTT_ERROR, false);
      rlog_i(tagMQTT, "Subscribed to: %s", event->topic);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      ledSysStateClear(SYSLED_MQTT_ERROR, false);
      rlog_i(tagMQTT, "Unsubscribed from: %s", event->topic);
      break;
    
    case MQTT_EVENT_PUBLISHED:
      ledSysStateClear(SYSLED_MQTT_ERROR, false);
      if (event->data_len > MQTT_LOG_PAYLOAD_LIMIT) {
        rlog_i(tagMQTT, "Published: %s [ %d bytes ]", event->topic, event->data_len);
      } else {
        rlog_i(tagMQTT, "Published: %s [ %s ]", event->topic, event->data);
      };
      break;
    
    case MQTT_EVENT_DATA:
      paramsMqttIncomingMessage(event->topic, event->data, event->data_len);
      break;
    
    case MQTT_EVENT_ERROR:
      ledSysStateSet(SYSLED_MQTT_ERROR, false);
      if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        rlog_e(tagMQTT, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
        rlog_e(tagMQTT, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
        rlog_e(tagMQTT, "Last captured errno: %d (%s)",  event->error_handle->esp_transport_sock_errno, strerror(event->error_handle->esp_transport_sock_errno));
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_ERROR, strerror(event->error_handle->esp_transport_sock_errno), event->error_handle->esp_transport_sock_errno);
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
      } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        rlog_e(tagMQTT, "Connection refused, error: 0x%x", event->error_handle->connect_return_code);
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_ERROR, "connection refused", event->error_handle->connect_return_code);
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
      } else {
        rlog_e(tagMQTT, "Unknown error type: 0x%x", event->error_handle->error_type);
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
          tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_ERROR, "unknown error type", event->error_handle->error_type);
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_MQTT_STATUS_TG_NOTIFY
      };
      break;

    default:
      rlog_w(tagMQTT, "Other event id: %d", event->event_id); 
      break;
  };
  return ESP_OK;
};

static void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  mqttEventHandlerCb((esp_mqtt_event_handle_t)event_data);
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Task routines -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttTaskCreate() 
{
  if (!_mqttClient) {
    esp_err_t err = ESP_OK;

    _mqttConnected = false;
    _mqttConnCnt = 0;

    // Configuring broker parameters
    static esp_mqtt_client_config_t _mqttCfg;
    memset(&_mqttCfg, 0, sizeof(_mqttCfg));

    _mqttCfg.host = CONFIG_MQTT_HOST;
    #ifdef CONFIG_MQTT_USERNAME
    _mqttCfg.username = CONFIG_MQTT_USERNAME;
    #ifdef CONFIG_MQTT_PASSWORD
    _mqttCfg.password = CONFIG_MQTT_PASSWORD;
    #endif // CONFIG_MQTT_PASSWORD
    #endif // CONFIG_MQTT_USERNAME
    #ifdef CONFIG_MQTT_CLIENTID
    _mqttCfg.client_id = CONFIG_MQTT_CLIENTID;
    #endif // CONFIG_MQTT_CLIENTID
    _mqttCfg.network_timeout_ms = CONFIG_MQTT_TIMEOUT;
    _mqttCfg.reconnect_timeout_ms = CONFIG_MQTT_RECONNECT;
    _mqttCfg.keepalive = CONFIG_MQTT_KEEP_ALIVE;
    _mqttCfg.disable_keepalive = false;
    _mqttCfg.buffer_size = CONFIG_MQTT_READ_BUFFER_SIZE;
    _mqttCfg.out_buffer_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
    _mqttCfg.task_prio = CONFIG_MQTT_CLIENT_PRIORITY;
    _mqttCfg.task_stack = CONFIG_MQTT_CLIENT_STACK_SIZE;
    _mqttCfg.disable_auto_reconnect = !CONFIG_MQTT_AUTO_RECONNECT;
    _mqttCfg.disable_clean_session = !CONFIG_MQTT_CLEAN_SESSION;
    _mqttCfg.use_global_ca_store = false;
    #ifdef CONFIG_MQTT_STATUS_LWT
    _mqttStatusTopic = mqttGetTopic1(CONFIG_MQTT_STATUS_TOPIC);
    _mqttCfg.lwt_topic = _mqttStatusTopic;
    _mqttCfg.lwt_msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
    _mqttCfg.lwt_msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
    _mqttCfg.lwt_qos = CONFIG_MQTT_STATUS_QOS;
    _mqttCfg.lwt_retain = CONFIG_MQTT_STATUS_RETAINED;
    #endif // CONFIG_MQTT_STATUS_LWT
    
    #if CONFIG_MQTT_TLS_ENABLE
    _mqttCfg.transport = MQTT_TRANSPORT_OVER_SSL;
    _mqttCfg.port = CONFIG_MQTT_PORT_TLS;
    _mqttCfg.cert_pem = (const char *)mqtt_broker_pem_start;
    _mqttCfg.skip_cert_common_name_check = CONFIG_MQTT_TLS_NAME_CHECK;
    #else
    _mqttCfg.transport = MQTT_TRANSPORT_OVER_TCP;
    _mqttCfg.port = CONFIG_MQTT_PORT_TCP;
    #endif // CONFIG_MQTT_TLS_ENABLE
      
    // Launching the client
    _mqttClient = esp_mqtt_client_init(&_mqttCfg);
    if (_mqttClient) {
      err = esp_mqtt_client_register_event(_mqttClient, MQTT_EVENT_ANY, mqttEventHandler, _mqttClient);
      if (err != ESP_OK) {
        rlog_e(tagMQTT, "Failed to register event handler!");
        ledSysStateSet(SYSLED_ERROR, false);
        return false;
      };
      err = esp_mqtt_client_start(_mqttClient); 
      if (err != ESP_OK) {
        rlog_e(tagMQTT, "Failed to start MQTT client!");
        ledSysStateSet(SYSLED_ERROR, false);
        return false;
      };
      rlog_d(tagMQTT, "Task [ MQTT_CLIENT ] was created");
      return true;
    } else {
      rlog_e(tagMQTT, "Failed to create MQTT client!");
      ledSysStateSet(SYSLED_ERROR, false);
      return false;
    };
  } else {
    return mqttTaskResume(); 
  };
}

bool mqttTaskDelete()
{
  if (_mqttClient) {
    esp_err_t err = esp_mqtt_client_destroy(_mqttClient);
    if (err != ESP_OK) {
      rlog_e(tagMQTT, "Failed to destroy MQTT client!");
      ledSysStateSet(SYSLED_ERROR, false);
      return false;
    };
    _mqttClient = nullptr;

    #ifdef CONFIG_MQTT_STATUS_LWT
    if (_mqttStatusTopic) {
      free(_mqttStatusTopic);
      _mqttStatusTopic = nullptr;
    };
    #endif // CONFIG_MQTT_STATUS_LWT

    rlog_d(tagMQTT, "Task [ MQTT_CLIENT ] was deleted");
    return true;
  };
  return false;
}

bool mqttTaskSuspend()
{
  if (_mqttClient) {
    esp_err_t err = esp_mqtt_client_stop(_mqttClient);
    if (err != ESP_OK) {
      rlog_e(tagMQTT, "Failed to stop MQTT client!");
      ledSysStateSet(SYSLED_ERROR, false);
      return false;
    };
    rlog_d(tagMQTT, "Task [ MQTT_CLIENT ] was stopped");
    return true;
  } else {
    rlog_w(tagMQTT, "Task [ MQTT_CLIENT ] not found");
    return false;
  };
}

bool mqttTaskResume()
{
  if (_mqttClient) {
    esp_err_t err = esp_mqtt_client_start(_mqttClient);
    if (err != ESP_OK) {
      rlog_e(tagMQTT, "Failed to start MQTT client!");
      ledSysStateSet(SYSLED_ERROR, false);
      return false;
    };
    rlog_d(tagMQTT, "Task [ MQTT_CLIENT ] was started");
    return true;
  } else {
    rlog_w(tagMQTT, "Task [ MQTT_CLIENT ] not found");
    return false;
  };
}

