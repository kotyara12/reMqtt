#include "reMqtt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/ssl.h"
#include <time.h>
#include "reTgSend.h"

static const char* logTAG = "MQTT";

#define MQTT_LOG_PAYLOAD_LIMIT 2048

#if defined(CONFIG_MQTT1_TYPE) && CONFIG_MQTT1_TLS_ENABLED && !defined(CONFIG_MQTT1_TLS_STORAGE)
  #define CONFIG_MQTT1_TLS_STORAGE TLS_CERT_BUFFER
#endif // CONFIG_MQTT1_TLS_STORAGE

#if defined(CONFIG_MQTT2_TYPE) && CONFIG_MQTT2_TLS_ENABLED && !defined(CONFIG_MQTT2_TLS_STORAGE)
  #define CONFIG_MQTT2_TLS_STORAGE TLS_CERT_BUFFER
#endif // CONFIG_MQTT2_TLS_STORAGE

#if defined(CONFIG_MQTT1_TYPE) && CONFIG_MQTT1_TLS_ENABLED && (CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_BUFFER)
  extern const uint8_t mqtt1_broker_pem_start[] asm(CONFIG_MQTT1_TLS_PEM_START);
  extern const uint8_t mqtt1_broker_pem_end[]   asm(CONFIG_MQTT1_TLS_PEM_END); 
#endif // CONFIG_MQTT1_TLS_ENABLED
#if defined(CONFIG_MQTT2_TYPE) && CONFIG_MQTT2_TLS_ENABLED && (CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_BUFFER)
  extern const uint8_t mqtt2_broker_pem_start[] asm(CONFIG_MQTT2_TLS_PEM_START);
  extern const uint8_t mqtt2_broker_pem_end[]   asm(CONFIG_MQTT2_TLS_PEM_END); 
#endif // CONFIG_MQTT2_TLS_ENABLED

static esp_mqtt_client_handle_t _mqttClient = nullptr;
static re_mqtt_event_data_t _mqttData;
static uint32_t _mqttConnAttempt = 0;

// Forward declarations
esp_err_t mqttClientCreate();
esp_err_t mqttClientRestart();
esp_err_t mqttClientStop();
esp_err_t mqttClientDestroy();

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Status bits -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static const uint32_t MQTTCLI_STARTED            = BIT0;
static const uint32_t MQTTCLI_INET_AVAILABLED    = BIT1;
static const uint32_t MQTTCLI_SERVER1_AVAILABLED = BIT2;
static const uint32_t MQTTCLI_SERVER2_AVAILABLED = BIT3;
static const uint32_t MQTTCLI_SERVER2_ACTIVE     = BIT4;
static const uint32_t MQTTCLI_CONNECTED          = BIT5;
static const uint32_t MQTTCLI_ERROR              = BIT6;

static EventGroupHandle_t _mqttStates = nullptr;
#if CONFIG_MQTT_STATIC_ALLOCATION
  StaticEventGroup_t _mqttBufStates;
#endif // CONFIG_MQTT_STATIC_ALLOCATION

bool mqttStatesInit() 
{
  if (_mqttStates == nullptr) {
    #if CONFIG_MQTT_STATIC_ALLOCATION
      _mqttStates = xEventGroupCreateStatic(&_mqttBufStates);
    #else
      _mqttStates = xEventGroupCreate();
    #endif // CONFIG_MQTT_STATIC_ALLOCATION
    if (_mqttStates != nullptr) {
      xEventGroupClearBits(_mqttStates, 0x00FFFFFFU);
      xEventGroupSetBits(_mqttStates, MQTTCLI_INET_AVAILABLED | MQTTCLI_SERVER1_AVAILABLED | MQTTCLI_SERVER2_AVAILABLED);
    } else {
      rlog_e(logTAG, "Failed to create event group!");
    };
  };
  return _mqttStates != nullptr;
}

void mqttStatesFree() 
{
  if (_mqttStates) {
    vEventGroupDelete(_mqttStates);
    _mqttStates = nullptr;
  };
}

EventBits_t mqttStatesGet() 
{
  if (_mqttStates) {
    return xEventGroupGetBits(_mqttStates);
  };
  rlog_e(logTAG, "Failed to get status bits, event group is null!");
  return 0;
}

bool mqttStatesCheck(EventBits_t bits, const bool clearOnExit) 
{
  if (_mqttStates) {
    if (clearOnExit) {
      return (xEventGroupClearBits(_mqttStates, bits) & bits) == bits;
    } else {
      return (xEventGroupGetBits(_mqttStates) & bits) == bits;
    };
  };
  rlog_e(logTAG, "Failed to check status bits: %X, event group is null!", bits);
  return false;
}

bool mqttStatesClear(EventBits_t bits)
{
  if (!_mqttStates) {
    rlog_e(logTAG, "Failed to set status bits: %X, event group is null!", bits);
    return false;
  };
  EventBits_t prevClear = xEventGroupClearBits(_mqttStates, bits);
  if ((prevClear & bits) != 0) {
    EventBits_t afterClear = xEventGroupGetBits(_mqttStates);
    if ((afterClear & bits) != 0) {
      rlog_e(logTAG, "Failed to clear status bits: %X, current value: %X", bits, afterClear);
      return false;
    };
  };
  return true;
}

bool mqttStatesSet(EventBits_t bits)
{
  if (!_mqttStates) {
    rlog_e(logTAG, "Failed to set status bits: %X, event group is null!", bits);
    return false;
  };
  EventBits_t afterSet = xEventGroupSetBits(_mqttStates, bits);
  if ((afterSet & bits) != bits) {
    rlog_e(logTAG, "Failed to set status bits: %X, current value: %X", bits, afterSet);
    return false;
  };
  return true;
}

bool mqttStatesSetBit(EventBits_t bit, bool state)
{
  if (state) {
    return mqttStatesSet(bit);
  } else {
    return mqttStatesClear(bit);
  };
}

EventBits_t mqttStatesWait(EventBits_t bits, BaseType_t clearOnExit, BaseType_t waitAllBits, TickType_t timeout)
{
  if (_mqttStates) {
    return xEventGroupWaitBits(_mqttStates, bits, clearOnExit, waitAllBits, timeout) & bits; 
  };  
  return 0;
}

EventBits_t mqttStatesWaitMs(EventBits_t bits, BaseType_t clearOnExit, BaseType_t waitAllBits, TickType_t timeout)
{
  if (_mqttStates) {
    if (timeout == 0) {
      return xEventGroupWaitBits(_mqttStates, bits, clearOnExit, waitAllBits, portMAX_DELAY) & bits; 
    }
    else {
      return xEventGroupWaitBits(_mqttStates, bits, clearOnExit, waitAllBits, pdMS_TO_TICKS(timeout)) & bits; 
    };
  };  
  return 0;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------- Return to main server timer ---------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if defined(CONFIG_MQTT2_TYPE) && defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)

#include "esp_timer.h"

esp_timer_handle_t _mqttBackToPrimaryTimer = nullptr;

bool mqttServer1SetAvailable(bool newAvailable);
void mqttBackToPrimaryTimerEnd(void* arg)
{
  mqttServer1SetAvailable(true); 
}

bool mqttBackToPrimaryTimerInit()
{
  if (_mqttBackToPrimaryTimer == nullptr) {
    esp_timer_create_args_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "mqtt_b2p";
    cfg.skip_unhandled_events = false;
    cfg.callback = mqttBackToPrimaryTimerEnd;
    RE_OK_CHECK(esp_timer_create(&cfg, &_mqttBackToPrimaryTimer), return false);
    rlog_i(logTAG, "The return timer to the main MQTT server is created");
  };
  return true;
}

bool mqttBackToPrimaryTimerStop()
{
  if ((_mqttBackToPrimaryTimer != nullptr) && esp_timer_is_active(_mqttBackToPrimaryTimer)) {
    RE_OK_CHECK(esp_timer_stop(_mqttBackToPrimaryTimer), return false);
    rlog_i(logTAG, "The return timer to the main MQTT server was stopped");
  };  
  return true;
}

bool mqttBackToPrimaryTimerFree()
{
  if (_mqttBackToPrimaryTimer != nullptr) {
    mqttBackToPrimaryTimerStop();
    RE_OK_CHECK(esp_timer_delete(_mqttBackToPrimaryTimer), return false);
    rlog_i(logTAG, "The return timer to the main MQTT server is deleted");
  };
  return true;
}

bool mqttBackToPrimaryTimerStart()
{
  if (_mqttBackToPrimaryTimer == nullptr) {
    if (!mqttBackToPrimaryTimerInit()) {
      return false;
    };
  };
  mqttBackToPrimaryTimerStop();
  RE_OK_CHECK(esp_timer_start_once(_mqttBackToPrimaryTimer, 60000000UL*CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES), return false);
  rlog_i(logTAG, "The return timer to the main MQTT server was started");
  return true;
}

#else

bool mqttBackToPrimaryTimerInit()
{
  return true;
}

bool mqttBackToPrimaryTimerFree()
{
  return true;
}

bool mqttBackToPrimaryTimerStart()
{
  return true;
}

bool mqttBackToPrimaryTimerStop()
{
  return true;
}

#endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Routines --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttIsConnected() 
{
  return wifiIsConnected() && (_mqttClient) && mqttStatesCheck(MQTTCLI_STARTED | MQTTCLI_CONNECTED, false);
}

bool mqttIsPrimary()
{
  return !mqttStatesCheck(MQTTCLI_SERVER2_ACTIVE, false);
}

int mqttGetOutboxSize()
{
  return esp_mqtt_client_get_outbox_size(_mqttClient);
}

void mqttErrorEventSend(const char* message, const char* object)
{
  mqttStatesSet(MQTTCLI_ERROR);
  if (message) {
    if (object) {
      char* err_msg = malloc_stringf(message, object);
      if (err_msg) {
        eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, (void*)err_msg, strlen(err_msg)+1, portMAX_DELAY);
        free(err_msg);
      };
    } else {
      eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, (void*)message, strlen(message)+1, portMAX_DELAY);
    };
  } else {
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, nullptr, 0, portMAX_DELAY);
  };
}

void mqttErrorEventSendCode(const char* message, const char* object, esp_err_t error_code)
{
  mqttStatesSet(MQTTCLI_ERROR);
  if (message) {
    char* err_msg = nullptr;
    if (object) {
      err_msg = malloc_stringf(message, object, error_code, esp_err_to_name(error_code));
    } else {
      err_msg = malloc_stringf(message, error_code, esp_err_to_name(error_code));
    };
    if (err_msg) {
      eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, (void*)err_msg, strlen(err_msg)+1, portMAX_DELAY);
      free(err_msg);
    };
  };
}

void mqttErrorEventClear()
{
  if (mqttStatesCheck(MQTTCLI_ERROR, true)) {
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR_CLEAR, nullptr, 0, portMAX_DELAY);
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ Publish system status ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO

static char* _mqttTopicStatus = nullptr;

char* mqttTopicStatusCreate(const bool primary)
{
  if (_mqttTopicStatus) free(_mqttTopicStatus);
  _mqttTopicStatus = mqttGetTopicDevice1(primary, CONFIG_MQTT_STATUS_LOCAL, CONFIG_MQTT_STATUS_TOPIC);
  if (_mqttTopicStatus) {
    rlog_i(logTAG, "Generated topic for publishing system status: [ %s ]", _mqttTopicStatus);
  } else {
    rlog_i(logTAG, "Failed to denerate topic for publishing system status");
  };
  return _mqttTopicStatus;
}

char* mqttTopicStatusGet()
{
  if (!_mqttTopicStatus) {
    if (mqttStatesCheck(MQTTCLI_CONNECTED, false)) {
      mqttTopicStatusCreate(_mqttData.primary);
    };
  };
  return _mqttTopicStatus;
}

void mqttTopicStatusFree()
{
  if (_mqttTopicStatus) free(_mqttTopicStatus);
  _mqttTopicStatus = nullptr;
  rlog_d(logTAG, "Topic for publishing system status has been scrapped");
}

#endif // CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Server selection ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

// Connect to server with new parameters
bool mqttServer1isLocal()
{
  return CONFIG_MQTT1_TYPE > 0;
}

bool mqttServer1Enabled()
{
  return mqttStatesCheck(MQTTCLI_SERVER1_AVAILABLED, false) && (mqttServer1isLocal() || mqttStatesCheck(MQTTCLI_INET_AVAILABLED, false));
}

#ifdef CONFIG_MQTT2_TYPE

bool mqttServer2isLocal()
{
  return CONFIG_MQTT2_TYPE > 0;
}

bool mqttServer2Enabled()
{
  return mqttStatesCheck(MQTTCLI_SERVER2_AVAILABLED, false) && (mqttServer2isLocal() || mqttStatesCheck(MQTTCLI_INET_AVAILABLED, false));
}

bool mqttServer1Activate()
{
  if (mqttStatesCheck(MQTTCLI_SERVER2_ACTIVE, false) || (_mqttClient == nullptr) || !mqttStatesCheck(MQTTCLI_STARTED, false)) {
    rlog_i(logTAG, "Primary MQTT broker selected");
    mqttBackToPrimaryTimerStop();
    mqttStatesClear(MQTTCLI_SERVER2_ACTIVE);
    if (_mqttClient) {
      if (mqttStatesCheck(MQTTCLI_STARTED, false)) {
        // The client is already running - can only be restarted through the event loop (from another task)
        return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SERVER_PRIMARY, nullptr, 0, portMAX_DELAY);
      } else {
        // The client is not running yet - you can just run it from this task
        return mqttClientRestart() == ESP_OK;
      };
    } else {
      // The client has not yet been created
      return mqttClientCreate() == ESP_OK;
    };
  };
  return true;
}

bool mqttServer2Activate()
{
  if (!mqttStatesCheck(MQTTCLI_SERVER2_ACTIVE, false) || (_mqttClient == nullptr) || !mqttStatesCheck(MQTTCLI_STARTED, false)) {
    rlog_i(logTAG, "Reserved MQTT broker selected");
    mqttBackToPrimaryTimerStart();
    mqttStatesSet(MQTTCLI_SERVER2_ACTIVE);
    if (_mqttClient) {
      if (mqttStatesCheck(MQTTCLI_STARTED, false)) {
        // The client is already running - can only be restarted through the event loop (from another task)
        return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SERVER_RESERVED, nullptr, 0, portMAX_DELAY);
      } else {
        // The client is not running yet - you can just run it from this task
        return mqttClientRestart() == ESP_OK;
      };
    } else {
      // The client has not yet been created
      return mqttClientCreate() == ESP_OK;
    };
  };
  return true;
}

// Setting network availability and starting the client, depending on which server is currently available
bool mqttServerSelectAuto()
{
  if (mqttServer1Enabled()) {
    return mqttServer1Activate();
  } else {
    if (mqttServer2Enabled()) {
      return mqttServer2Activate();
    } else {
      if (_mqttClient && mqttStatesCheck(MQTTCLI_STARTED, false)) {
        // Send event to suspend service
        return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SELF_STOP, nullptr, 0, portMAX_DELAY);
      };
    };
  };
  return false;
}

// Server status change (by ping)
bool mqttServer2SetAvailable(bool newAvailable)
{
  if (mqttStatesCheck(MQTTCLI_SERVER2_AVAILABLED, false) != newAvailable) {
    mqttStatesSetBit(MQTTCLI_SERVER2_AVAILABLED, newAvailable);
    return mqttServerSelectAuto();
  };
  return false;
}

#else

bool mqttServerSelectAuto()
{
  if (mqttServer1Enabled()) {
    mqttStatesClear(MQTTCLI_SERVER2_ACTIVE);
    if (_mqttClient) {
      // The client is not running yet - you can just run it from this task
      return mqttClientRestart() == ESP_OK;
    } else {
      // The client has not yet been created
      return mqttClientCreate() == ESP_OK;
    };
  } else {
    // Send event to suspend service
    return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SELF_STOP, nullptr, 0, portMAX_DELAY);
  };
}

#endif // CONFIG_MQTT2_TYPE

// Server status change (by ping)
bool mqttServer1SetAvailable(bool newAvailable)
{
  if (mqttStatesCheck(MQTTCLI_SERVER1_AVAILABLED, false) != newAvailable) {
    mqttStatesSetBit(MQTTCLI_SERVER1_AVAILABLED, newAvailable);
    return mqttServerSelectAuto();
  };
  return false;
}

// Server status change (by internet)
bool mqttServerSetInetAvailable(bool internetAvailable)
{
  mqttStatesSetBit(MQTTCLI_INET_AVAILABLED, internetAvailable);
  return mqttServerSelectAuto();
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Subscribe -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttSubscribe(const char *topic, int qos)
{
  if (mqttStatesCheck(MQTTCLI_CONNECTED, false) && (topic != nullptr)) {
    if (esp_mqtt_client_subscribe(_mqttClient, topic, qos) == -1) {
      rlog_e(logTAG, "Failed to subscribe to topic \"%s\"", topic);
      mqttErrorEventSend("Failed to subscribe to topic \"%s\"", topic);
      return false;
    };
    rlog_i(logTAG, "Subscribed to: \"%s\"", topic);
    return true;
  };
  return false;
}

bool mqttUnsubscribe(const char *topic)
{
  if (mqttStatesCheck(MQTTCLI_CONNECTED, false) && (topic != nullptr)) {
    if (esp_mqtt_client_unsubscribe(_mqttClient, topic) == -1) {
      rlog_e(logTAG, "Failed to unsubscribe from topic \"%s\"", topic);
      mqttErrorEventSend("Failed to unsubscribe from topic \"%s\"", topic);
      return false;
    };
    rlog_i(logTAG, "Unsubscribed from: \"%s\"", topic);
    return true;
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ Publish --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

esp_err_t mqttPublish(char *topic, char *payload, int qos, bool retained, bool free_topic, bool free_payload)
{
  esp_err_t err = ESP_ERR_INVALID_ARG;

  if (topic != nullptr) {
    size_t payload_len;
    if (payload == nullptr) {
      payload_len = 0;
    } else {
      payload_len = strlen(payload);
    };

    #if defined(CONFIG_MQTT_MAX_OUTBOX_SIZE) && (CONFIG_MQTT_MAX_OUTBOX_SIZE > 0)
      bool _enqueueOutbox = esp_mqtt_client_get_outbox_size(_mqttClient) < CONFIG_MQTT_MAX_OUTBOX_SIZE;
    #else
      bool _enqueueOutbox = true;
    #endif // CONFIG_MQTT_MAX_OUTBOX_SIZE

    #if defined(CONFIG_MQTT_MAX_OUTBOX_MESSAGE_SIZE) && (CONFIG_MQTT_MAX_OUTBOX_MESSAGE_SIZE > 0)
      bool _enqueueMessage = payload_len < CONFIG_MQTT_MAX_OUTBOX_MESSAGE_SIZE;
    #else
      bool _enqueueMessage = true;
    #endif // CONFIG_MQTT_MAX_OUTBOX_MESSAGE_SIZE

    if (mqttStatesCheck(MQTTCLI_CONNECTED, false)) {
      if (_enqueueOutbox && _enqueueMessage) {
        esp_mqtt_client_enqueue(_mqttClient, topic, payload, payload_len, qos, retained, true) > -1 ? err = ESP_OK : err = ESP_FAIL;
      } else {
        esp_mqtt_client_publish(_mqttClient, topic, payload, payload_len, qos, retained) > -1 ? err = ESP_OK : err = ESP_FAIL;
      };
    } else {
      if (_enqueueOutbox && _enqueueMessage) {
        esp_mqtt_client_enqueue(_mqttClient, topic, payload, payload_len, qos, retained, true) > -1 ? err = ESP_OK : err = ESP_FAIL;
      } else {
        err = ESP_ERR_INVALID_STATE;
      };
    };

    if (err == ESP_OK) {
      if (payload == nullptr) {
        rlog_i(logTAG, "Publish to topic \"%s\": NULL [ 0 bytes ]", topic);
      } else if (strlen(payload) > MQTT_LOG_PAYLOAD_LIMIT) {
        rlog_i(logTAG, "Publish to topic \"%s\": [ %d bytes ]", topic, strlen(payload));
      } else {
        rlog_i(logTAG, "Publish to topic \"%s\": %s", topic, payload);
      };
    } else {
      rlog_e(logTAG, "Failed to publish to topic \"%s\": %d, %s", topic, err, esp_err_to_name(err));
      mqttErrorEventSendCode("Failed to publish to topic \"%s\": %d, %s", topic, err);
    };
  };

  if (free_topic && (topic != nullptr)) free(topic);
  if (free_payload && (payload != nullptr)) free(payload);
  return err;
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Event callback ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  // esp_mqtt_client_handle_t client = data->client;
  esp_mqtt_event_handle_t data = (esp_mqtt_event_handle_t)event_data;
  static char* str_value = nullptr;
  static re_mqtt_incoming_data_t in_buffer;
  memset(&in_buffer, 0, sizeof(re_mqtt_incoming_data_t));

  switch (data->event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
      _mqttConnAttempt++;
      mqttStatesClear(MQTTCLI_CONNECTED);
      if (_mqttConnAttempt > 1) {
        rlog_w(logTAG, "Attempt # %d to connect to MQTT broker [ %s : %d ]...", _mqttConnAttempt, _mqttData.host, _mqttData.port);
      } else {
        rlog_i(logTAG, "Attempt # %d to connect to MQTT broker [ %s : %d ]...", _mqttConnAttempt, _mqttData.host, _mqttData.port);
      };
      #if CONFIG_SYSLED_MQTT_ACTIVITY
        ledSysActivity();
      #endif // CONFIG_SYSLED_MQTT_ACTIVITY
    break;

    case MQTT_EVENT_CONNECTED:
      _mqttConnAttempt = 0;
      mqttStatesSet(MQTTCLI_CONNECTED);
      rlog_i(logTAG, "Connection to MQTT broker [ %s : %d ] established", _mqttData.host, _mqttData.port);
      // Repost event to main event loop
      eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONNECTED, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
      mqttErrorEventClear();
      // Publish ONLINE static status
      #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO
        mqttPublish(mqttTopicStatusGet(), (char*)CONFIG_MQTT_STATUS_ONLINE_PAYLOAD, 
          CONFIG_MQTT_STATUS_QOS, CONFIG_MQTT_STATUS_RETAINED, false, false);
      #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO
      break;

    case MQTT_EVENT_DISCONNECTED:
      mqttErrorEventSend(nullptr, nullptr);
      if (mqttStatesCheck(MQTTCLI_CONNECTED, false)) {
        // The connection has already been established before, the connection is lost
        mqttStatesClear(MQTTCLI_CONNECTED);
        rlog_w(logTAG, "Lost connection to MQTT broker [ %s : %d ]", _mqttData.host, _mqttData.port);
        // Repost event to main event loop
        eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_LOST, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
      } else {
        // Failed to establish connection
        if (_mqttConnAttempt == CONFIG_MQTT_CONNECT_ATTEMPTS) {
          // Repost event to main event loop
          eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_FAILED, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
          _mqttConnAttempt = 0;
          #ifdef CONFIG_MQTT2_TYPE
            // Switching to the another server - disable current server
            if (mqttStatesCheck(MQTTCLI_SERVER2_ACTIVE, false)) {
              mqttServer2SetAvailable(false);
            } else {
              mqttServer1SetAvailable(false);
            };
          #endif // CONFIG_MQTT2_TYPE
        };
      };
      break;

    case MQTT_EVENT_SUBSCRIBED:
    case MQTT_EVENT_UNSUBSCRIBED:
    case MQTT_EVENT_PUBLISHED:
      mqttErrorEventClear();
      #if CONFIG_SYSLED_MQTT_ACTIVITY
        ledSysActivity();
      #endif // CONFIG_SYSLED_MQTT_ACTIVITY
      break;
    
    case MQTT_EVENT_DATA:
      if (event_data) {
        if (data->current_data_offset == 0) {
          if (in_buffer.topic) free(in_buffer.topic);
          in_buffer.topic = nullptr;
          if (in_buffer.data) free(in_buffer.data);
          in_buffer.data = (char*)esp_calloc(1, data->total_data_len+1);
        };
        if (in_buffer.data) {
          memcpy(in_buffer.data+data->current_data_offset, data->data, data->data_len);
          if (data->current_data_offset + data->data_len == data->total_data_len) {
            in_buffer.topic = malloc_stringl(data->topic, data->topic_len);
            if (in_buffer.topic) {
              in_buffer.topic_len = data->topic_len;
              in_buffer.data_len = data->total_data_len;
              rlog_d(logTAG, "Incoming message \"%.*s\": [%s]", data->topic_len, data->topic, in_buffer.data);
              // Repost string to main event loop
              eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_INCOMING_DATA, &in_buffer, sizeof(in_buffer), portMAX_DELAY);
              #if CONFIG_SYSLED_MQTT_ACTIVITY
                ledSysActivity();
              #endif // CONFIG_SYSLED_MQTT_ACTIVITY
            };
          };
        };
      };
      break;
    
    case MQTT_EVENT_ERROR:
      if (event_data) {
        rlog_e(logTAG, "MQTT client error!");
        // Generate error message
        if (data->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          str_value = malloc_stringf("Transport error: %d\n  - %s\nESP_TLS error:   0x%X\nTLS stack error: 0x%X", 
            data->error_handle->esp_transport_sock_errno, strerror(data->error_handle->esp_transport_sock_errno),
            data->error_handle->esp_tls_last_esp_err, data->error_handle->esp_tls_stack_err);
        } else if (data->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
          str_value = malloc_stringf("Connection refused, error: 0x%x", 
            data->error_handle->connect_return_code);
        } else {
          str_value = malloc_stringf("Unknown error type: 0x%x", 
            data->error_handle->error_type);
        };
        // Repost event to main event loop
        mqttErrorEventSend(str_value, nullptr);
      };
      #if CONFIG_SYSLED_MQTT_ACTIVITY
        ledSysActivity();
      #endif // CONFIG_SYSLED_MQTT_ACTIVITY
      break;

    default:
      rlog_w(logTAG, "Other event id: %d", data->event_id); 
      break;
  };
  // Free resources
  if (str_value) {
    free(str_value);
    str_value = nullptr;
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Configuration -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void mqttSetConfigPrimary(esp_mqtt_client_config_t * mqttCfg)
{
  // Host
  _mqttData.primary = true;
  #if CONFIG_MQTT1_TYPE == 0
    _mqttData.local = false;
  #else
    _mqttData.local = true;
  #endif // CONFIG_MQTT1_TYPE == 0

  memset(_mqttData.host, 0, sizeof(_mqttData.host));
  #if CONFIG_MQTT1_TYPE == 2
    char *_host = wifiGetGatewayIP();
    if (_host) {
      strcpy(_mqttData.host, _host);
      free(_host);
    };
  #else
    strcpy(_mqttData.host, CONFIG_MQTT1_HOST);
  #endif // CONFIG_MQTT1_TYPE == 2

  #if ESP_IDF_VERSION_MAJOR < 5
    // Hostname
    mqttCfg->host = _mqttData.host;

    // Port and transport
    #if CONFIG_MQTT1_TLS_ENABLED
      _mqttData.port = CONFIG_MQTT1_PORT_TLS;
      mqttCfg->port = CONFIG_MQTT1_PORT_TLS;
      mqttCfg->skip_cert_common_name_check = false;
      mqttCfg->transport = MQTT_TRANSPORT_OVER_SSL;
      #if CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_BUFFER
        mqttCfg->cert_pem = (const char *)mqtt1_broker_pem_start;
        mqttCfg->cert_len = mqtt1_broker_pem_end - mqtt1_broker_pem_start;
        mqttCfg->use_global_ca_store = false;
      #elif CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_GLOBAL
        mqttCfg->use_global_ca_store = true;
      #elif CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_BUNDLE
        mqttCfg->crt_bundle_attach = esp_crt_bundle_attach;
        mqttCfg->use_global_ca_store = false;
      #endif // CONFIG_MQTT2_TLS_STORAGE
    #else
      _mqttData.port = CONFIG_MQTT1_PORT_TCP;
      mqttCfg->transport = MQTT_TRANSPORT_OVER_TCP;
      mqttCfg->port = CONFIG_MQTT1_PORT_TCP;
    #endif // CONFIG_MQTT1_TLS_ENABLED

    // Credentials
    #ifdef CONFIG_MQTT1_USERNAME
      mqttCfg->username = CONFIG_MQTT1_USERNAME;
      #ifdef CONFIG_MQTT1_PASSWORD
        mqttCfg->password = CONFIG_MQTT1_PASSWORD;
      #endif // CONFIG_MQTT1_PASSWORD
    #endif // CONFIG_MQTT1_USERNAME

    // ClientId, if needed. Otherwise ClientId will be generated automatically
    #ifdef CONFIG_MQTT1_CLIENTID
      mqttCfg->client_id = CONFIG_MQTT1_CLIENTID;
    #endif // CONFIG_MQTT_CLIENTID

    // Network parameters
    mqttCfg->network_timeout_ms = CONFIG_MQTT1_TIMEOUT;
    mqttCfg->reconnect_timeout_ms = CONFIG_MQTT1_RECONNECT;
    mqttCfg->disable_auto_reconnect = !CONFIG_MQTT1_AUTO_RECONNECT;

    // Session parameters
    mqttCfg->disable_clean_session = !CONFIG_MQTT1_CLEAN_SESSION;
    mqttCfg->keepalive = CONFIG_MQTT1_KEEP_ALIVE;
    mqttCfg->disable_keepalive = false;

    // LWT
    #if CONFIG_MQTT_STATUS_LWT
      mqttCfg->lwt_topic = mqttTopicStatusCreate(true);
      mqttCfg->lwt_msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
      mqttCfg->lwt_msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
      mqttCfg->lwt_qos = CONFIG_MQTT_STATUS_QOS;
      mqttCfg->lwt_retain = CONFIG_MQTT_STATUS_RETAINED;
    #endif // CONFIG_MQTT_STATUS_LWT

    // Task & buffers
    mqttCfg->buffer_size = CONFIG_MQTT_READ_BUFFER_SIZE;
    mqttCfg->out_buffer_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
    mqttCfg->task_prio = CONFIG_TASK_PRIORITY_MQTT_CLIENT;
    mqttCfg->task_stack = CONFIG_MQTT_CLIENT_STACK_SIZE;
  #else
    // Hostname
    mqttCfg->broker.address.hostname = _mqttData.host;

    // Port and transport
    #if CONFIG_MQTT1_TLS_ENABLED
      _mqttData.port = CONFIG_MQTT1_PORT_TLS;
      mqttCfg->broker.address.port = CONFIG_MQTT1_PORT_TLS;
      mqttCfg->broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
      mqttCfg->broker.verification.skip_cert_common_name_check = false;
      #if CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_BUFFER
        mqttCfg->broker.verification.certificate = (const char *)mqtt1_broker_pem_start;
        mqttCfg->broker.verification.certificate_len = mqtt1_broker_pem_end - mqtt1_broker_pem_start;
        mqttCfg->broker.verification.use_global_ca_store = false;
      #elif CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_GLOBAL
        mqttCfg->broker.verification.use_global_ca_store = true;
      #elif CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_BUNDLE
        mqttCfg->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        mqttCfg->broker.verification.use_global_ca_store = false;
      #endif // CONFIG_MQTT2_TLS_STORAGE
    #else
      _mqttData.port = CONFIG_MQTT1_PORT_TCP;
      mqttCfg->broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
      mqttCfg->broker.address.port = CONFIG_MQTT1_PORT_TCP;
    #endif // CONFIG_MQTT1_TLS_ENABLED

    // Credentials
    #ifdef CONFIG_MQTT1_USERNAME
      mqttCfg->credentials.username = CONFIG_MQTT1_USERNAME;
      #ifdef CONFIG_MQTT1_PASSWORD
        mqttCfg->credentials.authentication.password = CONFIG_MQTT1_PASSWORD;
      #endif // CONFIG_MQTT1_PASSWORD
    #endif // CONFIG_MQTT1_USERNAME

    // ClientId, if needed. Otherwise ClientId will be generated automatically
    #ifdef CONFIG_MQTT1_CLIENTID
      mqttCfg->credentials.client_id = CONFIG_MQTT1_CLIENTID;
    #endif // CONFIG_MQTT_CLIENTID

    // Network parameters
    mqttCfg->network.timeout_ms = CONFIG_MQTT1_TIMEOUT;
    mqttCfg->network.reconnect_timeout_ms = CONFIG_MQTT1_RECONNECT;
    mqttCfg->network.disable_auto_reconnect = !CONFIG_MQTT1_AUTO_RECONNECT;

    // Session parameters
    mqttCfg->session.disable_clean_session = !CONFIG_MQTT1_CLEAN_SESSION;
    mqttCfg->session.keepalive = CONFIG_MQTT1_KEEP_ALIVE;
    mqttCfg->session.disable_keepalive = false;

    // LWT
    #if CONFIG_MQTT_STATUS_LWT
      mqttCfg->session.last_will.topic = mqttTopicStatusCreate(true);
      mqttCfg->session.last_will.msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
      mqttCfg->session.last_will.msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
      mqttCfg->session.last_will.qos = CONFIG_MQTT_STATUS_QOS;
      mqttCfg->session.last_will.retain = CONFIG_MQTT_STATUS_RETAINED;
    #endif // CONFIG_MQTT_STATUS_LWT

    // Task & buffers
    mqttCfg->task.priority = CONFIG_TASK_PRIORITY_MQTT_CLIENT;
    mqttCfg->task.stack_size = CONFIG_MQTT_CLIENT_STACK_SIZE;
    mqttCfg->buffer.size = CONFIG_MQTT_READ_BUFFER_SIZE;
    mqttCfg->buffer.out_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
  #endif // #if ESP_IDF_VERSION_MAJOR
}

#ifdef CONFIG_MQTT2_TYPE

void mqttSetConfigReserved(esp_mqtt_client_config_t * mqttCfg)
{
  // Host
  _mqttData.primary = false;
  #if CONFIG_MQTT2_TYPE == 0
    _mqttData.local = false;
  #else
    _mqttData.local = true;
  #endif // CONFIG_MQTT2_TYPE == 0
  
  memset(_mqttData.host, 0, sizeof(_mqttData.host));
  #if CONFIG_MQTT2_TYPE == 2
    char *_host = wifiGetGatewayIP();
    if (_host) {
      strcpy(_mqttData.host, _host);
      free(_host);
    };
  #else
    strcpy(_mqttData.host, CONFIG_MQTT2_HOST);
  #endif // CONFIG_MQTT1_TYPE == 2
  
  #if ESP_IDF_VERSION_MAJOR < 5
    // Hostname
    mqttCfg->host = _mqttData.host;
    
    // Port and transport
    #if CONFIG_MQTT2_TLS_ENABLED
      _mqttData.port = CONFIG_MQTT2_PORT_TLS;
      mqttCfg->port = CONFIG_MQTT2_PORT_TLS;
      mqttCfg->skip_cert_common_name_check = false;
      mqttCfg->transport = MQTT_TRANSPORT_OVER_SSL;
      #if CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_BUFFER
        mqttCfg->cert_pem = (const char *)mqtt2_broker_pem_start;
        mqttCfg->cert_len = mqtt2_broker_pem_end - mqtt2_broker_pem_start;
        mqttCfg->use_global_ca_store = false;
      #elif CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_GLOBAL
        mqttCfg->use_global_ca_store = true;
      #elif CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_BUNDLE
        mqttCfg->crt_bundle_attach = esp_crt_bundle_attach;
        mqttCfg->use_global_ca_store = false;
      #endif // CONFIG_MQTT2_TLS_STORAGE
    #else
      _mqttData.port = CONFIG_MQTT2_PORT_TCP;
      mqttCfg->transport = MQTT_TRANSPORT_OVER_TCP;
      mqttCfg->port = CONFIG_MQTT2_PORT_TCP;
    #endif // CONFIG_MQTT2_TLS_ENABLED

    // Credentials
    #ifdef CONFIG_MQTT2_USERNAME
      mqttCfg->username = CONFIG_MQTT2_USERNAME;
      #ifdef CONFIG_MQTT2_PASSWORD
        mqttCfg->password = CONFIG_MQTT2_PASSWORD;
      #endif // CONFIG_MQTT2_PASSWORD
    #endif // CONFIG_MQTT2_USERNAME

    // ClientId, if needed. Otherwise ClientId will be generated automatically
    #ifdef CONFIG_MQTT2_CLIENTID
      mqttCfg->client_id = CONFIG_MQTT2_CLIENTID;
    #endif // CONFIG_MQTT_CLIENTID

    // Network parameters
    mqttCfg->network_timeout_ms = CONFIG_MQTT2_TIMEOUT;
    mqttCfg->reconnect_timeout_ms = CONFIG_MQTT2_RECONNECT;
    mqttCfg->disable_auto_reconnect = !CONFIG_MQTT2_AUTO_RECONNECT;

    // Session parameters
    mqttCfg->disable_clean_session = !CONFIG_MQTT2_CLEAN_SESSION;
    mqttCfg->keepalive = CONFIG_MQTT2_KEEP_ALIVE;
    mqttCfg->disable_keepalive = false;

    // LWT
    #if CONFIG_MQTT_STATUS_LWT
      mqttCfg->lwt_topic = mqttTopicStatusCreate(true);
      mqttCfg->lwt_msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
      mqttCfg->lwt_msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
      mqttCfg->lwt_qos = CONFIG_MQTT_STATUS_QOS;
      mqttCfg->lwt_retain = CONFIG_MQTT_STATUS_RETAINED;
    #endif // CONFIG_MQTT_STATUS_LWT

    // Task & buffers
    mqttCfg->buffer_size = CONFIG_MQTT_READ_BUFFER_SIZE;
    mqttCfg->out_buffer_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
    mqttCfg->task_prio = CONFIG_TASK_PRIORITY_MQTT_CLIENT;
    mqttCfg->task_stack = CONFIG_MQTT_CLIENT_STACK_SIZE;
  #else
    // Hostname
    mqttCfg->broker.address.hostname = _mqttData.host;

    // Port and transport
    #if CONFIG_MQTT2_TLS_ENABLED
      _mqttData.port = CONFIG_MQTT2_PORT_TLS;
      mqttCfg->broker.address.port = CONFIG_MQTT2_PORT_TLS;
      mqttCfg->broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
      mqttCfg->broker.verification.skip_cert_common_name_check = false;
      #if CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_BUFFER
        mqttCfg->broker.verification.certificate = (const char *)mqtt2_broker_pem_start;
        mqttCfg->broker.verification.certificate_len = mqtt2_broker_pem_end - mqtt2_broker_pem_start;
        mqttCfg->broker.verification.use_global_ca_store = false;
      #elif CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_GLOBAL
        mqttCfg->broker.verification.use_global_ca_store = true;
      #elif CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_BUNDLE
        mqttCfg->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        mqttCfg->broker.verification.use_global_ca_store = false;
      #endif // CONFIG_MQTT2_TLS_STORAGE
    #else
      _mqttData.port = CONFIG_MQTT2_PORT_TCP;
      mqttCfg->broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
      mqttCfg->broker.address.port = CONFIG_MQTT2_PORT_TCP;
    #endif // CONFIG_MQTT2_TLS_ENABLED

    // Credentials
    #ifdef CONFIG_MQTT2_USERNAME
      mqttCfg->credentials.username = CONFIG_MQTT2_USERNAME;
      #ifdef CONFIG_MQTT2_PASSWORD
        mqttCfg->credentials.authentication.password = CONFIG_MQTT2_PASSWORD;
      #endif // CONFIG_MQTT2_PASSWORD
    #endif // CONFIG_MQTT2_USERNAME

    // ClientId, if needed. Otherwise ClientId will be generated automatically
    #ifdef CONFIG_MQTT2_CLIENTID
      mqttCfg->credentials.client_id = CONFIG_MQTT2_CLIENTID;
    #endif // CONFIG_MQTT_CLIENTID

    // Network parameters
    mqttCfg->network.timeout_ms = CONFIG_MQTT2_TIMEOUT;
    mqttCfg->network.reconnect_timeout_ms = CONFIG_MQTT2_RECONNECT;
    mqttCfg->network.disable_auto_reconnect = !CONFIG_MQTT2_AUTO_RECONNECT;

    // Session parameters
    mqttCfg->session.disable_clean_session = !CONFIG_MQTT2_CLEAN_SESSION;
    mqttCfg->session.keepalive = CONFIG_MQTT2_KEEP_ALIVE;
    mqttCfg->session.disable_keepalive = false;

    // LWT
    #if CONFIG_MQTT_STATUS_LWT
      mqttCfg->session.last_will.topic = mqttTopicStatusCreate(false);
      mqttCfg->session.last_will.msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
      mqttCfg->session.last_will.msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
      mqttCfg->session.last_will.qos = CONFIG_MQTT_STATUS_QOS;
      mqttCfg->session.last_will.retain = CONFIG_MQTT_STATUS_RETAINED;
    #endif // CONFIG_MQTT_STATUS_LWT

    // Task & buffers
    mqttCfg->task.priority = CONFIG_TASK_PRIORITY_MQTT_CLIENT;
    mqttCfg->task.stack_size = CONFIG_MQTT_CLIENT_STACK_SIZE;
    mqttCfg->buffer.size = CONFIG_MQTT_READ_BUFFER_SIZE;
    mqttCfg->buffer.out_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
  #endif // #if ESP_IDF_VERSION_MAJOR
}

#endif // CONFIG_MQTT2_TYPE

esp_err_t mqttInitConfig(esp_mqtt_client_config_t * mqttCfg)
{
  RE_MEM_CHECK_EVENT(mqttCfg, return ESP_ERR_INVALID_ARG);

  _mqttConnAttempt = 0;
  memset(&_mqttData, 0, sizeof(_mqttData));
  memset(mqttCfg, 0, sizeof(esp_mqtt_client_config_t));
  mqttStatesClear(MQTTCLI_STARTED | MQTTCLI_CONNECTED);

  #ifdef CONFIG_MQTT2_TYPE
    mqttStatesCheck(MQTTCLI_SERVER2_ACTIVE, false) ? mqttSetConfigReserved(mqttCfg) : mqttSetConfigPrimary(mqttCfg);
  #else
    mqttSetConfigPrimary(mqttCfg);
  #endif // CONFIG_MQTT2_TYPE

  #if ESP_IDF_VERSION_MAJOR < 5
    RE_MEM_CHECK_EVENT(mqttCfg->host, return ESP_ERR_INVALID_ARG);
    #if CONFIG_MQTT_STATUS_LWT
      RE_MEM_CHECK_EVENT(mqttCfg->lwt_topic, return ESP_ERR_INVALID_ARG);
    #endif // CONFIG_MQTT_STATUS_LWT
  #else
    RE_MEM_CHECK_EVENT(mqttCfg->broker.address.hostname, return ESP_ERR_INVALID_ARG);
    #if CONFIG_MQTT_STATUS_LWT
      RE_MEM_CHECK_EVENT(mqttCfg->session.last_will.topic, return ESP_ERR_INVALID_ARG);
    #endif // CONFIG_MQTT_STATUS_LWT
  #endif // ESP_IDF_VERSION_MAJOR

  return ESP_OK;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Client routines ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

esp_err_t mqttClientCreate() 
{
  if (_mqttClient == nullptr) {
    rlog_i(logTAG, "Create MQTT client...");

    // Set configuration
    esp_mqtt_client_config_t _mqttCfg;
    esp_err_t err = mqttInitConfig(&_mqttCfg);
    if (err != ESP_OK) return err;

    // Init client
    _mqttClient = esp_mqtt_client_init(&_mqttCfg);
    if (_mqttClient == nullptr) {
      rlog_e(logTAG, "Failed to create task [ MQTT_CLIENT ]: out of memory");
      mqttErrorEventSendCode("Failed to create task [ MQTT_CLIENT ]: %d %s", nullptr, ESP_ERR_NO_MEM);
      return ESP_ERR_NO_MEM;
    };
    
    err = esp_mqtt_client_register_event(_mqttClient, MQTT_EVENT_ANY, mqttEventHandler, _mqttClient);
    if (err != ESP_OK) {
      mqttErrorEventSendCode("Failed to create task (re) [ MQTT_CLIENT ]: %d %s", nullptr, err);
      rlog_e(logTAG, "Failed to create task (re) [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
    };

    // Start client
    err = esp_mqtt_client_start(_mqttClient);
    if (err == ESP_OK) {
      mqttStatesSet(MQTTCLI_STARTED);
      rlog_i(logTAG, "Task [ MQTT_CLIENT ] was started");
    } else {
      mqttErrorEventSendCode("Failed to start task [ MQTT_CLIENT ]: %d %s", nullptr, err);
      rlog_e(logTAG, "Failed to start task [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
    };

    return err;
  } else {
    return mqttClientRestart();
  };
}

esp_err_t mqttClientRestart()
{
  rlog_w(logTAG, "Restart MQTT client...");

  // Close the current connection if it exists
  esp_err_t err = mqttClientStop();
  if (err != ESP_OK) return err;

  // Set new configuration
  esp_mqtt_client_config_t _mqttCfg;
  err = mqttInitConfig(&_mqttCfg);
  if (err != ESP_OK) return err;

  err = esp_mqtt_set_config(_mqttClient, &_mqttCfg);
  if (err != ESP_OK) {
    mqttErrorEventSendCode("Failed to configure MQTT client [ MQTT_CLIENT ]: %d %s", nullptr, err);
    rlog_e(logTAG, "Failed to configure MQTT client [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
    return err;
  };
    
  // Start client
  err = esp_mqtt_client_start(_mqttClient);
  if (err == ESP_OK) {
    mqttStatesSet(MQTTCLI_STARTED);
    rlog_i(logTAG, "Task [ MQTT_CLIENT ] was started");
  } else {
    mqttErrorEventSendCode("Failed to start task [ MQTT_CLIENT ]: %d %s", nullptr, err);
    rlog_e(logTAG, "Failed to start task [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
  };
  return err;
}

esp_err_t mqttClientStop()
{
  if (_mqttClient) {
    if (mqttStatesCheck(MQTTCLI_STARTED, false)) {
      rlog_w(logTAG, "Stop MQTT client...");
      esp_err_t err = esp_mqtt_client_stop(_mqttClient);
      if (err == ESP_OK) {
        if (mqttStatesCheck(MQTTCLI_CONNECTED, false)) {
          eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_LOST, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
        };
        rlog_i(logTAG, "Task [ MQTT_CLIENT ] was stopped");
        // Reset variables
        _mqttConnAttempt = 0;
        mqttStatesClear(MQTTCLI_STARTED | MQTTCLI_CONNECTED);
        #if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
          mqttTopicStatusFree();
        #endif // CONFIG_MQTT_STATUS_LWT
      } else {
        mqttErrorEventSendCode("Failed to stop task [ MQTT_CLIENT ]: %d %s", nullptr, err);
        rlog_e(logTAG, "Failed to stop task [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
      };
      return err;
    };
    return ESP_OK;
  };
  return ESP_FAIL;
}

esp_err_t mqttClientDestroy()
{
  if (_mqttClient) {
    rlog_w(logTAG, "Destroy MQTT client...");
    // Disсonnect from server and stop task
    mqttClientStop();
    // Destoy client
    esp_err_t err = esp_mqtt_client_destroy(_mqttClient);
    if (err == ESP_OK) {
      // Reset variables
      _mqttClient = nullptr;
      rlog_i(logTAG, "Task [ MQTT_CLIENT ] was deleted");
    } else {
      rlog_e(logTAG, "Failed to destroy task [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
    };
  };
  return ESP_OK;
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Task routines ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttTaskInit()
{
  return mqttStatesInit() && mqttBackToPrimaryTimerInit();
}

bool mqttTaskStart(bool createSuspended)
{
  if (createSuspended) {
    return mqttTaskInit() && mqttEventHandlerRegister();
  } else {
    return mqttTaskInit() && mqttEventHandlerRegister() && mqttServerSelectAuto();
  };
}

bool mqttTaskRestart()
{
  return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_COLD_RESTART, nullptr, 0, portMAX_DELAY);
}

bool mqttTaskFree()
{
  if (mqttClientDestroy()) {
    mqttEventHandlerUnregister();
    mqttBackToPrimaryTimerFree();
    mqttStatesFree();
    return true;
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- WiFi event handler -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void mqttWiFiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // STA connected, Internet access not checked
  if (event_id == RE_WIFI_STA_GOT_IP) {
    // rlog_v(logTAG, "Event received: RE_WIFI_STA_GOT_IP");
    mqttServerSetInetAvailable(false);
  }
  // STA connected and Internet access is available
  else if (event_id == RE_WIFI_STA_PING_OK) {
    // rlog_v(logTAG, "Event received: RE_WIFI_STA_PING_OK");
    mqttServerSetInetAvailable(true);
  }
  // Internet access lost
  else if (event_id == RE_WIFI_STA_PING_FAILED) {
    // rlog_v(logTAG, "Event received: RE_WIFI_STA_PING_FAILED");
    mqttServerSetInetAvailable(false);
  }
  // STA disconnected
  else if ((event_id == RE_WIFI_STA_DISCONNECTED) || (event_id == RE_WIFI_STA_STOPPED)) {
    // rlog_v(logTAG, "Event received: RE_WIFI_STA_DISCONNECTED");
    if (mqttStatesCheck(MQTTCLI_STARTED, false)) {
      mqttClientStop();
    };
  };
}

static void mqttSelfEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_MQTT_SELF_STOP) {
    mqttClientStop();
  } else if ((event_id == RE_MQTT_SERVER_PRIMARY) || (event_id == RE_MQTT_SERVER_RESERVED)) {
    uint8_t cntTry = 0;
    while (cntTry < 60) {
      cntTry++;
      if (mqttClientStop() == ESP_OK) break;
      vTaskDelay(pdMS_TO_TICKS(1000));
    };
    mqttClientRestart();
  } else if (event_id == RE_MQTT_COLD_RESTART) {
    uint8_t cntTry = 0;
    while (cntTry < 60) {
      cntTry++;
      if (mqttClientDestroy() == ESP_OK) break;
      vTaskDelay(pdMS_TO_TICKS(1000));
    };
    mqttClientCreate();
  };
}

#if defined(CONFIG_MQTT1_PING_CHECK) && CONFIG_MQTT1_PING_CHECK
static void mqttPing1EventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // rlog_d("tgEVT", "Recieved PING event: %s %d", event_base, event_id);
  ping_inet_data_t* data = (ping_inet_data_t*)event_data;

  // Broker 1 is available
  if (event_id == RE_PING_MQTT1_AVAILABLE) {
    rlog_v(logTAG, "Event received: RE_PING_MQTT1_AVAILABLE");
    mqttServer1SetAvailable(true);
  }
  // Broker 1 is unavailable
  else if (event_id == RE_PING_MQTT1_UNAVAILABLE) {
    rlog_v(logTAG, "Event received: RE_PING_MQTT1_UNAVAILABLE");
    mqttServer1SetAvailable(false);
  }
}
#endif // CONFIG_MQTT1_PING_CHECK

#if defined(CONFIG_MQTT2_TYPE) && defined(CONFIG_MQTT2_PING_CHECK) && CONFIG_MQTT2_PING_CHECK
static void mqttPing2EventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // rlog_d("tgEVT", "Recieved PING event: %s %d", event_base, event_id);
  ping_inet_data_t* data = (ping_inet_data_t*)event_data;

  // Broker 2 is available
  if (event_id == RE_PING_MQTT2_AVAILABLE) {
    rlog_v(logTAG, "Event received: RE_PING_MQTT2_AVAILABLE");
    mqttServer2SetAvailable(true);
  }
  // Broker 2 is unavailable
  else if (event_id == RE_PING_MQTT2_UNAVAILABLE) {
    rlog_v(logTAG, "Event received: RE_PING_MQTT2_UNAVAILABLE");
    mqttServer2SetAvailable(false);
  }
}
#endif // CONFIG_MQTT1_PING_CHECK

bool mqttEventHandlerRegister()
{
  rlog_d(logTAG, "Register MQTT event handlers...");
  return eventHandlerRegister(RE_WIFI_EVENTS, ESP_EVENT_ANY_ID, &mqttWiFiEventHandler, nullptr) 
      #if defined(CONFIG_MQTT1_PING_CHECK) && CONFIG_MQTT1_PING_CHECK
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT1_AVAILABLE, &mqttPing1EventHandler, nullptr)
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT1_UNAVAILABLE, &mqttPing1EventHandler, nullptr)
      #endif // CONFIG_MQTT1_PING_CHECK
      #if defined(CONFIG_MQTT2_TYPE) && defined(CONFIG_MQTT2_PING_CHECK) && CONFIG_MQTT2_PING_CHECK
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT2_AVAILABLE, &mqttPing2EventHandler, nullptr)
      && eventHandlerRegister(RE_PING_EVENTS, RE_PING_MQTT2_UNAVAILABLE, &mqttPing2EventHandler, nullptr)
      #endif // CONFIG_MQTT2_PING_CHECK
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_COLD_RESTART, &mqttSelfEventHandler, nullptr)
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_SELF_STOP, &mqttSelfEventHandler, nullptr)
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_SERVER_PRIMARY, &mqttSelfEventHandler, nullptr)
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_SERVER_RESERVED, &mqttSelfEventHandler, nullptr);
}

void mqttEventHandlerUnregister()
{
  rlog_d(logTAG, "Unregister MQTT event handlers...");
  eventHandlerUnregister(RE_WIFI_EVENTS, ESP_EVENT_ANY_ID, &mqttWiFiEventHandler);
  #if defined(CONFIG_MQTT1_PING_CHECK) && CONFIG_MQTT1_PING_CHECK
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_MQTT1_AVAILABLE, &mqttPing1EventHandler);
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_MQTT1_UNAVAILABLE, &mqttPing1EventHandler);
  #endif // CONFIG_MQTT1_PING_CHECK
  #if defined(CONFIG_MQTT2_TYPE) && defined(CONFIG_MQTT2_PING_CHECK) && CONFIG_MQTT2_PING_CHECK
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_MQTT2_AVAILABLE, &mqttPing2EventHandler);
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_MQTT2_UNAVAILABLE, &mqttPing2EventHandler);
  #endif // CONFIG_MQTT2_PING_CHECK
  eventHandlerUnregister(RE_MQTT_EVENTS, RE_MQTT_COLD_RESTART, &mqttSelfEventHandler);
  eventHandlerUnregister(RE_MQTT_EVENTS, RE_MQTT_SELF_STOP, &mqttSelfEventHandler);
  eventHandlerUnregister(RE_MQTT_EVENTS, RE_MQTT_SERVER_PRIMARY, &mqttSelfEventHandler);
  eventHandlerUnregister(RE_MQTT_EVENTS, RE_MQTT_SERVER_RESERVED, &mqttSelfEventHandler);
}
