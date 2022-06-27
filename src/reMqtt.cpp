#include "reMqtt.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

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

#if (defined(CONFIG_MQTT1_TYPE) && CONFIG_MQTT1_TLS_ENABLED && (CONFIG_MQTT1_TLS_STORAGE == TLS_CERT_BUNDLE)) || (defined(CONFIG_MQTT2_TYPE) && CONFIG_MQTT2_TLS_ENABLED && (CONFIG_MQTT2_TLS_STORAGE == TLS_CERT_BUNDLE))
  #include "esp_crt_bundle.h"
#endif // TLS_CERT_BUNDLE

typedef enum {
  MQTT_CLIENT_STOPPED   = 0,
  MQTT_CLIENT_STARTED   = 1,
  MQTT_CLIENT_SUSPENDED = 2
} mqtt_client_state_t;

static re_mqtt_event_data_t _mqttData;
static mqtt_client_state_t _mqttState = MQTT_CLIENT_STOPPED;
static bool _mqttError = false;
#if defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)
  static uint32_t _mqttBackToPrimarty = 0;
#endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES

#if CONFIG_MQTT_USE_LWMQTT_CLIENT
  #include <lwip/netdb.h>
  #include <mbedtls/certs.h>
  #include <mbedtls/ctr_drbg.h>
  #include <mbedtls/entropy.h>
  #include <mbedtls/error.h>
  #include <mbedtls/net_sockets.h>
  #include <mbedtls/platform.h>
  #include <mbedtls/ssl.h> 

  static SemaphoreHandle_t _mqttMutexMain = nullptr;
  #define ESP_MQTT_LOCK_MAIN() do { } while (xSemaphoreTake(_mqttMutexMain, portMAX_DELAY) != pdPASS)
  #define ESP_MQTT_UNLOCK_MAIN() xSemaphoreGive(_mqttMutexMain)

  static SemaphoreHandle_t _mqttMutexSelect = nullptr;
  #define ESP_MQTT_LOCK_SELECT() do { } while (xSemaphoreTake(_mqttMutexSelect, portMAX_DELAY) != pdPASS)
  #define ESP_MQTT_UNLOCK_SELECT() xSemaphoreGive(_mqttMutexSelect)

  typedef struct {
    uint32_t deadline;
  } esp_lwmqtt_timer_t; 

  typedef struct {
    int socket;
  } esp_lwmqtt_network_t; 

  typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_net_context socket;
    uint8_t *ca_buf;
    size_t ca_len;
    // bool verify;
  } esp_tls_lwmqtt_network_t; 

  typedef struct {
    const char *host;
    uint32_t port;
    const char *client_id;
    const char *username;
    const char *password;
    const char *lwt_topic;
    const char *lwt_msg;
    int lwt_qos;
    int lwt_retain;
    int lwt_msg_len;
    bool use_tls;
    const char *cert_pem;
    size_t cert_len;
    bool skip_cert_common_name_check;
    int reconnect_timeout_ms;
    int network_timeout_ms;
    bool disable_auto_reconnect;
    bool disable_clean_session;
  } esp_mqtt_client_config_t;

  typedef struct {
    void *buffer;
    lwmqtt_string_t topic;
    lwmqtt_message_t message;
  } esp_mqtt_incoming_event_t;

  #define CONFIG_MQTT_INCOMING_EVENT_SIZE sizeof(esp_mqtt_incoming_event_t*)

  static lwmqtt_client_t _mqttClient;
  static esp_mqtt_client_config_t _mqttConfig;
  static esp_lwmqtt_network_t _mqttNetwork;
  static esp_tls_lwmqtt_network_t _mqttTlsNetwork;

  static esp_lwmqtt_timer_t _mqtt_timer1, _mqtt_timer2;
  static char _mqtt_write_buffer[CONFIG_MQTT_WRITE_BUFFER_SIZE];
  static char _mqtt_read_buffer[CONFIG_MQTT_READ_BUFFER_SIZE];

  static const char* mqttTaskName = "lwmqtt_client";
  static TaskHandle_t _mqttTask = nullptr;
  static QueueHandle_t _mqttIncomingQueue = nullptr;

  #if CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
    StaticSemaphore_t _mqttMutexBufferMain;
    StaticSemaphore_t _mqttMutexBufferSelect;
    StaticQueue_t _mqttQueueBuffer;
    StaticTask_t _mqttTaskBuffer;
    StackType_t _mqttTaskStack[CONFIG_MQTT_CLIENT_STACK_SIZE];
    uint8_t _mqttQueueStorage[CONFIG_MQTT_INCOMING_QUEUE_SIZE * CONFIG_MQTT_INCOMING_EVENT_SIZE];
  #endif // CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
#else
  #define ESP_MQTT_LOCK_MAIN()
  #define ESP_MQTT_UNLOCK_MAIN()
  #define ESP_MQTT_LOCK_SELECT()
  #define ESP_MQTT_UNLOCK_SELECT()

  static esp_mqtt_client_handle_t _mqttClient = nullptr;
#endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Utilites --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttIsConnected() 
{
  return wifiIsConnected() && _mqttData.connected;
}

void mqttErrorEventSend(char* message)
{
  _mqttError = true;
  if (message) {
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, (void*)message, strlen(message)+1, portMAX_DELAY);
  } else {
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR, nullptr, 0, portMAX_DELAY);
  };
}

void mqttErrorEventSendCode(const char* message, esp_err_t error_code)
{
  char* err_msg = malloc_stringf(message, error_code, esp_err_to_name(error_code));
  mqttErrorEventSend(err_msg);
  if (err_msg != nullptr) free(err_msg);
}

void mqttErrorEventClear()
{
  if (_mqttError) {
    _mqttError = false;
    eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR_CLEAR, nullptr, 0, portMAX_DELAY);
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ Publish system status ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_STATUS_ONLINE_SYSINFO

static char* _mqttTopicStatus = NULL;

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
    if (_mqttData.connected) {
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
bool mqttClientStart();
bool mqttClientDisconnect();
bool mqttClientResume();
bool mqttClientStop();

static bool _mqttInternetAvailable = false;
static bool _mqttServer1Available = true;

bool mqttServer1isLocal()
{
  return CONFIG_MQTT1_TYPE > 0;
}

bool mqttServer1Enabled()
{
  // rlog_d(logTAG, "mqttServer1Enabled(): _mqttServer1Available=%d, _mqttInternetAvailable=%d", _mqttServer1Available, _mqttInternetAvailable);
  return _mqttServer1Available && (_mqttInternetAvailable || mqttServer1isLocal());
}

#ifdef CONFIG_MQTT2_TYPE

static bool _mqttPrimary = true;
static bool _mqttServer2Available = true;
static time_t _mqttServer2Activate = 0;

bool mqttServer2isLocal()
{
  return CONFIG_MQTT2_TYPE > 0;
}

bool mqttServer2Enabled()
{
  return _mqttServer2Available && (_mqttInternetAvailable || mqttServer2isLocal());
}

#if defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)
static void mqttTimeEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
#endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES

bool mqttServer1Start()
{
  _mqttPrimary = true;
  _mqttServer2Activate = 0;
  rlog_i(logTAG, "Primary MQTT broker selected");
  #if defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)
    _mqttBackToPrimarty = 0;
    eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return mqttClientStart();
}

bool mqttServer2Start()
{
  _mqttPrimary = false;
  _mqttServer2Activate = time(nullptr);
  rlog_i(logTAG, "Reserved MQTT broker selected");
  #if defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)
    _mqttBackToPrimarty = 0;
    eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler, nullptr);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return mqttClientStart();
}

bool mqttServer1Select()
{
  _mqttPrimary = true;
  _mqttServer2Activate = 0;
  rlog_w(logTAG, "Switching to primary MQTT broker");
  #if defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)
    _mqttBackToPrimarty = 0;
    eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SERVER_PRIMARY, nullptr, 0, portMAX_DELAY);
}

bool mqttServer2Select()
{
  _mqttPrimary = false;
  _mqttServer2Activate = time(nullptr);
  rlog_w(logTAG, "Switching to reserved MQTT broker");
  #if defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)
    _mqttBackToPrimarty = 0;
    eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &mqttTimeEventHandler, nullptr);
  #endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES
  return eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_SERVER_RESERVED, nullptr, 0, portMAX_DELAY);
}

bool mqttServer1Acivate()
{
  switch (_mqttState) {
    case MQTT_CLIENT_STOPPED:
      return mqttServer1Start();
      break;
    case MQTT_CLIENT_STARTED:
      if (!_mqttPrimary) {
        return mqttServer1Select();
      };
      break;
    case MQTT_CLIENT_SUSPENDED:
      if (_mqttPrimary) {
        return mqttClientResume();
      } else {
        return mqttServer1Select();
      };
      break;
  };
  return false;
}

bool mqttServer2Acivate()
{
  switch (_mqttState) {
    case MQTT_CLIENT_STOPPED:
      return mqttServer2Start();
      break;
    case MQTT_CLIENT_STARTED:
      if (_mqttPrimary) {
        return mqttServer2Select();
      };
      break;
    case MQTT_CLIENT_SUSPENDED:
      if (!_mqttPrimary) {
        return mqttClientResume();
      } else {
        return mqttServer2Select();
      };
      break;
  };
  return false;
}

// Setting network availability and starting the client, depending on which server is currently available
bool mqttServerSelectAuto()
{
  if (mqttServer1Enabled()) {
    return mqttServer1Acivate();
  } else {
    if (mqttServer2Enabled()) {
      return mqttServer2Acivate();
    } else {
      return mqttClientDisconnect();
    };
  };
  return false;
}

// Server status change (by ping)
bool mqttServer2SetAvailable(bool newAvailable)
{
  if (_mqttServer2Available != newAvailable) {
    _mqttServer2Available = newAvailable;
    return mqttServerSelectAuto();
  };
  return false;
}

#else

bool mqttServerSelectAuto()
{
  if (mqttServer1Enabled()) {
    switch (_mqttState) {
      case MQTT_CLIENT_STOPPED:
        return mqttClientStart();
      case MQTT_CLIENT_STARTED:
        return false;
      case MQTT_CLIENT_SUSPENDED:
        return mqttClientResume();
    };
  } else {
    if (_mqttState == MQTT_CLIENT_STARTED) {
      return mqttClientDisconnect();
    };
  };
  return false;
}

#endif // CONFIG_MQTT2_TYPE

// Server status change (by ping)
bool mqttServer1SetAvailable(bool newAvailable)
{
  if (_mqttServer1Available != newAvailable) {
    _mqttServer1Available = newAvailable;
    return mqttServerSelectAuto();
  };
  return false;
}

// Server status change (by internet)
bool mqttServerSelectInet(bool internetAvailable, bool updateServers)
{
  _mqttInternetAvailable = internetAvailable;
  if (updateServers) {
    _mqttServer1Available = internetAvailable || mqttServer1isLocal();
    #ifdef CONFIG_MQTT2_TYPE
    _mqttServer2Available = internetAvailable || mqttServer2isLocal();
    #endif // CONFIG_MQTT2_TYPE
  };
  return mqttServerSelectAuto();
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Subscribe -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttSubscribe(const char *topic, int qos)
{
  ESP_MQTT_LOCK_MAIN();

  if (_mqttData.connected && (topic != nullptr)) {
    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      lwmqtt_string_t lwt_topic = lwmqtt_string(topic);
      lwmqtt_err_t err = lwmqtt_subscribe(&_mqttClient, 1, &lwt_topic, (lwmqtt_qos_t*)&qos, _mqttConfig.network_timeout_ms);
      if (err != LWMQTT_SUCCESS) {
        rlog_e(logTAG, "Failed to subscribe to topic \"%s\": %d", topic, err);
        mqttErrorEventSend(nullptr);
        ESP_MQTT_UNLOCK_MAIN();
        return false;
      }
    #else
      int msg_id = esp_mqtt_client_subscribe(_mqttClient, topic, qos);
      if (msg_id == -1) {
        rlog_e(logTAG, "Failed to subscribe to topic \"%s\"", topic);
        mqttErrorEventSend(nullptr);
        ESP_MQTT_UNLOCK_MAIN();
        return false;
      };
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
    
    rlog_i(logTAG, "Subscribed to: \"%s\"", topic);
    ESP_MQTT_UNLOCK_MAIN();
    return true;
  };
  
  ESP_MQTT_UNLOCK_MAIN();
  return false;
}

bool mqttUnsubscribe(const char *topic)
{
  ESP_MQTT_LOCK_MAIN();

  if (_mqttData.connected && (topic != nullptr)) {
    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      lwmqtt_string_t lwt_topic = lwmqtt_string(topic);
      lwmqtt_err_t err = lwmqtt_unsubscribe(&_mqttClient, 1, &lwt_topic, _mqttConfig.network_timeout_ms);
      if (err != LWMQTT_SUCCESS) {
        rlog_e(logTAG, "Failed to unsubscribe from topic \"%s\": %d", topic, err);
        mqttErrorEventSend(nullptr);
        ESP_MQTT_UNLOCK_MAIN();
        return false;
      }
    #else
      int msg_id = esp_mqtt_client_unsubscribe(_mqttClient, topic);
      if (msg_id == -1) {
        rlog_e(logTAG, "Failed to unsubscribe from topic \"%s\"", topic);
        mqttErrorEventSend(nullptr);
        ESP_MQTT_UNLOCK_MAIN();
        return false;
      };
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
    
    rlog_i(logTAG, "Unsubscribed from: \"%s\"", topic);
    ESP_MQTT_UNLOCK_MAIN();
    return true;
  };

  ESP_MQTT_UNLOCK_MAIN();
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ Publish --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

esp_err_t mqttPublish(char *topic, char *payload, int qos, bool retained, bool free_topic, bool free_payload)
{
  esp_err_t err = ESP_ERR_INVALID_ARG;
  ESP_MQTT_LOCK_MAIN();

  if (topic != nullptr) {
    size_t payload_len;
    if (payload == nullptr) {
      payload_len = 0;
    } else {
      payload_len = strlen(payload);
    };

    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      // LWMQTT client
      if (_mqttData.connected) {
        lwmqtt_message_t message = {
          .qos = (lwmqtt_qos_t)qos,
          .retained = retained,
          .payload = (uint8_t*)payload,
          .payload_len = payload_len
        };
        err = (esp_err_t)lwmqtt_publish(&_mqttClient, lwmqtt_string(topic), message, _mqttConfig.network_timeout_ms);
      } else {
        err = ESP_ERR_INVALID_STATE;
      };
    #else
      // Built-in ESP-IDF client
      if (_mqttData.connected) {
        if ((payload_len < CONFIG_MQTT_MAX_OUTBOX_MESSAGE_SIZE) && (esp_mqtt_client_get_outbox_size(_mqttClient) < CONFIG_MQTT_MAX_OUTBOX_SIZE)) {
          esp_mqtt_client_enqueue(_mqttClient, topic, payload, payload_len, qos, retained, true) > -1 ? err = ESP_OK : err = ESP_FAIL;
        } else {
          esp_mqtt_client_publish(_mqttClient, topic, payload, payload_len, qos, retained) > -1 ? err = ESP_OK : err = ESP_FAIL;
        };
      } else {
        if ((payload_len < CONFIG_MQTT_MAX_OUTBOX_MESSAGE_SIZE) && (esp_mqtt_client_get_outbox_size(_mqttClient) < CONFIG_MQTT_MAX_OUTBOX_SIZE)) {
          esp_mqtt_client_enqueue(_mqttClient, topic, payload, payload_len, qos, retained, true) > -1 ? err = ESP_OK : err = ESP_FAIL;
        } else {
          err = ESP_ERR_INVALID_STATE;
        };
      };
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

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
      mqttErrorEventSend(nullptr);
    };
  };

  if (free_topic && (topic != nullptr)) free(topic);
  if (free_payload && (payload != nullptr)) free(payload);
  ESP_MQTT_UNLOCK_MAIN();
  return err;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- LWMQTT routines ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_USE_LWMQTT_CLIENT

  static void esp_tls_log(const char *name, int ret) {
    char str[256];
    mbedtls_strerror(ret, str, 256);
    rlog_e(logTAG, "%s: %s (%d)", name, str, ret);
  }

  void esp_lwmqtt_timer_set(void *ref, uint32_t timeout) {
    // cast timer reference
    esp_lwmqtt_timer_t *t = (esp_lwmqtt_timer_t *)ref;

    // set deadline
    t->deadline = (xTaskGetTickCount() * portTICK_PERIOD_MS) + timeout;
  }

  int32_t esp_lwmqtt_timer_get(void *ref) {
    // cast timer reference
    esp_lwmqtt_timer_t *t = (esp_lwmqtt_timer_t *)ref;

    return (int32_t)t->deadline - (int32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  }

  // Forward declarations
  void esp_lwmqtt_network_disconnect(esp_lwmqtt_network_t *n);
  void esp_tls_lwmqtt_network_disconnect(esp_tls_lwmqtt_network_t *n);

  // Connect

  lwmqtt_err_t esp_lwmqtt_network_connect(esp_lwmqtt_network_t *n, char *host, char *port) 
  {
    // disconnect if not already the case
    esp_lwmqtt_network_disconnect(n);

    // prepare hints
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    // lookup ip address (there is no way to configure a timeout)
    struct addrinfo *res;
    int r = getaddrinfo(host, port, &hints, &res);
    if (r != 0 || res == NULL) {
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // create socket
    n->socket = socket(res->ai_family, res->ai_socktype, 0);
    if (n->socket < 0) {
      freeaddrinfo(res);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // disable nagle's algorithm
    int flag = 1;
    r = setsockopt(n->socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));
    if (r < 0) {
      close(n->socket);
      freeaddrinfo(res);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // set socket to non-blocking
    r = fcntl(n->socket, F_SETFL, fcntl(n->socket, F_GETFL, 0) | O_NONBLOCK);
    if (r < 0) {
      close(n->socket);
      freeaddrinfo(res);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // connect socket
    r = connect(n->socket, res->ai_addr, res->ai_addrlen);
    if (r < 0 && errno != EINPROGRESS) {
      close(n->socket);
      freeaddrinfo(res);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // free address
    freeaddrinfo(res);

    return LWMQTT_SUCCESS;
  }

  lwmqtt_err_t esp_tls_lwmqtt_network_connect(esp_tls_lwmqtt_network_t *n, char *host, char *port) 
  {
    // disconnect if not already the case
    esp_tls_lwmqtt_network_disconnect(n);

    // initialize support structures
    mbedtls_net_init(&n->socket);
    mbedtls_ssl_init(&n->ssl);
    mbedtls_ssl_config_init(&n->conf);
    mbedtls_x509_crt_init(&n->cacert);
    mbedtls_ctr_drbg_init(&n->ctr_drbg);
    mbedtls_entropy_init(&n->entropy);

    // setup entropy source
    int ret = mbedtls_ctr_drbg_seed(&n->ctr_drbg, mbedtls_entropy_func, &n->entropy, NULL, 0);
    if (ret != 0) {
      esp_tls_log("mbedtls_ctr_drbg_seed", ret);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // parse ca certificate
    if (n->ca_buf) {
      ret = mbedtls_x509_crt_parse(&n->cacert, n->ca_buf, n->ca_len);
      if (ret != 0) {
        esp_tls_log("mbedtls_x509_crt_parse", ret);
        return LWMQTT_NETWORK_FAILED_CONNECT;
      }
    }

    // connect socket
    ret = mbedtls_net_connect(&n->socket, host, port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
      esp_tls_log("mbedtls_net_connect", ret);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // load defaults
    ret = mbedtls_ssl_config_defaults(&n->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
      esp_tls_log("mbedtls_ssl_config_defaults", ret);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // set ca certificate
    if (n->ca_buf) {
      mbedtls_ssl_conf_ca_chain(&n->conf, &n->cacert, NULL);
    }

    // set auth mode
    mbedtls_ssl_conf_authmode(&n->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

    // set rng callback
    mbedtls_ssl_conf_rng(&n->conf, mbedtls_ctr_drbg_random, &n->ctr_drbg);

    // setup ssl context
    ret = mbedtls_ssl_setup(&n->ssl, &n->conf);
    if (ret != 0) {
      esp_tls_log("mbedtls_ssl_setup", ret);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // set hostname
    ret = mbedtls_ssl_set_hostname(&n->ssl, host);
    if (ret != 0) {
      esp_tls_log("mbedtls_ssl_set_hostname", ret);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // set bio callbacks
    mbedtls_ssl_set_bio(&n->ssl, &n->socket, mbedtls_net_send, mbedtls_net_recv, NULL);

    // verify certificate if requested
    uint32_t flags = mbedtls_ssl_get_verify_result(&n->ssl);
    if (flags != 0) {
      char verify_buf[100] = {0};
      mbedtls_x509_crt_verify_info(verify_buf, sizeof(verify_buf), "  ! ", flags);
      rlog_e(logTAG, "%mbedtls_ssl_get_verify_result: %s (%d)", verify_buf, flags);
    }

    // perform handshake
    ret = mbedtls_ssl_handshake(&n->ssl);
    if (ret != 0) {
      esp_tls_log("mbedtls_ssl_handshake", ret);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    return LWMQTT_SUCCESS;
  }

  // Wait
  
  lwmqtt_err_t esp_lwmqtt_network_wait(esp_lwmqtt_network_t *n, bool *connected, uint32_t timeout) 
  {
    // prepare sets
    fd_set set;
    fd_set ex_set;
    FD_ZERO(&set);
    FD_ZERO(&ex_set);
    FD_SET(n->socket, &set);
    FD_SET(n->socket, &ex_set);

    // wait for data
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int result = select(n->socket + 1, NULL, &set, &ex_set, &t);
    if (result < 0 || FD_ISSET(n->socket, &ex_set)) {
      close(n->socket);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // set whether socket is connected
    *connected = result > 0;

    // set socket to blocking
    int r = fcntl(n->socket, F_SETFL, fcntl(n->socket, F_GETFL, 0) & (~O_NONBLOCK));
    if (r < 0) {
      close(n->socket);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    return LWMQTT_SUCCESS;
  }

  lwmqtt_err_t esp_tls_lwmqtt_network_wait(esp_tls_lwmqtt_network_t *n, bool *connected, uint32_t timeout) 
  {
    // prepare sets
    fd_set set;
    fd_set ex_set;
    FD_ZERO(&set);
    FD_ZERO(&ex_set);
    FD_SET(n->socket.fd, &set);
    FD_SET(n->socket.fd, &ex_set);

    // wait for data
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int result = select(n->socket.fd + 1, NULL, &set, &ex_set, &t);
    if (result < 0 || FD_ISSET(n->socket.fd, &ex_set)) {
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    // set whether socket is connected
    *connected = result > 0;

    // set socket to blocking
    int ret = mbedtls_net_set_block(&n->socket);
    if (ret < 0) {
      esp_tls_log("mbedtls_net_set_block", ret);
      return LWMQTT_NETWORK_FAILED_CONNECT;
    }

    return LWMQTT_SUCCESS;
  }

  // Disconnect

  void esp_lwmqtt_network_disconnect(esp_lwmqtt_network_t *n) 
  {
    // close socket if present
    if (n->socket) {
      close(n->socket);
      n->socket = 0;
    }
  }

  void esp_tls_lwmqtt_network_disconnect(esp_tls_lwmqtt_network_t *n) 
  {
    // check if network is available
    if (!n) {
      return;
    }

    // cleanup resources
    mbedtls_ssl_close_notify(&n->ssl);
    mbedtls_x509_crt_free(&n->cacert);
    mbedtls_entropy_free(&n->entropy);
    mbedtls_ssl_config_free(&n->conf);
    mbedtls_ctr_drbg_free(&n->ctr_drbg);
    mbedtls_ssl_free(&n->ssl);
    mbedtls_net_free(&n->socket);
  }

  // Select

  lwmqtt_err_t esp_lwmqtt_network_select(esp_lwmqtt_network_t *n, bool *available, uint32_t timeout) {
    // prepare sets
    fd_set set;
    fd_set ex_set;
    FD_ZERO(&set);
    FD_ZERO(&ex_set);
    FD_SET(n->socket, &set);
    FD_SET(n->socket, &ex_set);

    // wait for data
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int result = select(n->socket + 1, &set, NULL, &ex_set, &t);
    if (result < 0 || FD_ISSET(n->socket, &ex_set)) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // set whether data is available
    *available = result > 0;

    return LWMQTT_SUCCESS;
  }

  lwmqtt_err_t esp_tls_lwmqtt_network_select(esp_tls_lwmqtt_network_t *n, bool *available, uint32_t timeout) {
    // prepare sets
    fd_set set;
    fd_set ex_set;
    FD_ZERO(&set);
    FD_ZERO(&ex_set);
    FD_SET(n->socket.fd, &set);
    FD_SET(n->socket.fd, &ex_set);

    // wait for data
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int result = select(n->socket.fd + 1, &set, NULL, &ex_set, &t);
    if (result < 0 || FD_ISSET(n->socket.fd, &ex_set)) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // set whether data is available
    *available = result > 0;

    return LWMQTT_SUCCESS;
  }

  // Peek

  lwmqtt_err_t esp_lwmqtt_network_peek(esp_lwmqtt_network_t *n, size_t *available) {
    // check if socket is valid
    char buf;
    int ret = recv(n->socket, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (ret <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // get the available bytes on the socket
    int rc = ioctl(n->socket, FIONREAD, available);
    if (rc < 0) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    return LWMQTT_SUCCESS;
  }

  lwmqtt_err_t esp_tls_lwmqtt_network_peek(esp_tls_lwmqtt_network_t *n, size_t *available, uint32_t timeout) {
    // set timeout
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int rc = setsockopt(n->socket.fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof(t));
    if (rc < 0) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // set socket to non blocking
    int ret = mbedtls_net_set_nonblock(&n->socket);
    if (ret != 0) {
      esp_tls_log("mbedtls_net_set_nonblock", ret);
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // TODO: Directly peek on underlying socket?

    // check if socket is valid
    ret = mbedtls_ssl_read(&n->ssl, NULL, 0);
    if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      esp_tls_log("mbedtls_ssl_read", ret);
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // set available bytes
    *available = mbedtls_ssl_get_bytes_avail(&n->ssl);

    // set socket back to blocking
    ret = mbedtls_net_set_block(&n->socket);
    if (ret != 0) {
      esp_tls_log("mbedtls_net_set_block", ret);
      return LWMQTT_NETWORK_FAILED_READ;
    }

    return LWMQTT_SUCCESS;
  }

  // Read

  lwmqtt_err_t esp_lwmqtt_network_read(void *ref, uint8_t *buffer, size_t len, size_t *received, uint32_t timeout) {
    // cast network reference
    esp_lwmqtt_network_t *n = (esp_lwmqtt_network_t *)ref;

    // set timeout
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int rc = setsockopt(n->socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof(t));
    if (rc < 0) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // read from socket
    int bytes = read(n->socket, buffer, len);
    if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return LWMQTT_SUCCESS;
    } else if (bytes <= 0) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // increment counter
    *received += bytes;

    return LWMQTT_SUCCESS;
  }

  lwmqtt_err_t esp_tls_lwmqtt_network_read(void *ref, uint8_t *buffer, size_t len, size_t *received, uint32_t timeout) {
    // cast network reference
    esp_tls_lwmqtt_network_t *n = (esp_tls_lwmqtt_network_t *)ref;

    // set timeout
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int rc = setsockopt(n->socket.fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof(t));
    if (rc < 0) {
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // read from socket
    int ret = mbedtls_ssl_read(&n->ssl, buffer, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      return LWMQTT_SUCCESS;
    } else if (ret <= 0) {
      esp_tls_log("mbedtls_ssl_read", ret);
      return LWMQTT_NETWORK_FAILED_READ;
    }

    // increment counter
    *received += ret;

    return LWMQTT_SUCCESS;
  }

  // Write

  lwmqtt_err_t esp_lwmqtt_network_write(void *ref, uint8_t *buffer, size_t len, size_t *sent, uint32_t timeout) {
    // cast network reference
    esp_lwmqtt_network_t *n = (esp_lwmqtt_network_t *)ref;

    // set timeout
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int rc = setsockopt(n->socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&t, sizeof(t));
    if (rc < 0) {
      return LWMQTT_NETWORK_FAILED_WRITE;
    }

    // write to socket
    int bytes = write(n->socket, buffer, len);
    if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return LWMQTT_SUCCESS;
    } else if (bytes < 0) {
      return LWMQTT_NETWORK_FAILED_WRITE;
    }

    // increment counter
    *sent += bytes;

    return LWMQTT_SUCCESS;
  } 

  lwmqtt_err_t esp_tls_lwmqtt_network_write(void *ref, uint8_t *buffer, size_t len, size_t *sent, uint32_t timeout) {
    // cast network reference
    esp_tls_lwmqtt_network_t *n = (esp_tls_lwmqtt_network_t *)ref;

    // set timeout
    struct timeval t = {.tv_sec = (long)(timeout / 1000), .tv_usec = (long)((timeout % 1000) * 1000)};
    int rc = setsockopt(n->socket.fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&t, sizeof(t));
    if (rc < 0) {
      return LWMQTT_NETWORK_FAILED_WRITE;
    }

    // write to socket
    int ret = mbedtls_ssl_write(&n->ssl, buffer, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      return LWMQTT_SUCCESS;
    } else if (ret < 0) {
      esp_tls_log("mbedtls_ssl_write", ret);
      return LWMQTT_NETWORK_FAILED_WRITE;
    }

    // increment counter
    *sent += ret;

    return LWMQTT_SUCCESS;
  } 

#endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Event callback ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_USE_LWMQTT_CLIENT
  static void mqttTaskExec(void *pvParameters)
  { 
    while (true) {
    };
    mqttTaskFree();
  }
#else
  static void mqttEventHandler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
  {
    // esp_mqtt_client_handle_t client = data->client;
    esp_mqtt_event_handle_t data = (esp_mqtt_event_handle_t)event_data;
    static char* str_value = nullptr;
    static re_mqtt_incoming_data_t in_buffer;
    memset(&in_buffer, 0, sizeof(re_mqtt_incoming_data_t));

    switch (data->event_id) {
      case MQTT_EVENT_BEFORE_CONNECT:
        _mqttData.conn_attempt++;
        rlog_i(logTAG, "Attempt # %d to connect to MQTT broker [ %s : %d ]...", _mqttData.conn_attempt, _mqttData.host, _mqttData.port);
        #if CONFIG_SYSLED_MQTT_ACTIVITY
        ledSysActivity();
        #endif // CONFIG_SYSLED_MQTT_ACTIVITY
      break;

      case MQTT_EVENT_CONNECTED:
        _mqttData.connected = true;
        _mqttData.conn_attempt = 0;
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
        mqttErrorEventSend(nullptr);
        if (_mqttData.connected) {
          // The connection has already been established before, the connection is lost
          _mqttData.connected = false;
          rlog_w(logTAG, "Lost connection to MQTT broker [ %s : %d ]", _mqttData.host, _mqttData.port);
          // Repost event to main event loop
          eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_LOST, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
        } else {
          // Failed to establish connection
          if (_mqttData.conn_attempt == CONFIG_MQTT_CONNECT_ATTEMPTS) {
            // Repost event to main event loop
            eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_FAILED, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
            #ifdef CONFIG_MQTT2_TYPE
            // Switching to the another server - disable current server
            if (_mqttPrimary) {
              mqttServer1SetAvailable(false);
            } else {
              mqttServer2SetAvailable(false);
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
          // Generate error message
          if (data->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            str_value = malloc_stringf("transport error %d (%s) | ESP_TLS error code: 0x%x | TLS stack error: 0x%x", 
              data->error_handle->esp_transport_sock_errno, strerror(data->error_handle->esp_transport_sock_errno),
              data->error_handle->esp_tls_last_esp_err, data->error_handle->esp_tls_stack_err);
          } else if (data->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            str_value = malloc_stringf("connection refused, error: 0x%x", 
              data->error_handle->connect_return_code);
          } else {
            str_value = malloc_stringf("unknown error type: 0x%x", 
              data->error_handle->error_type);
          };
          // Repost event to main event loop
          mqttErrorEventSend(str_value);
          if (str_value) rlog_e(logTAG, "MQTT client error: %s", str_value);
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
#endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Configuration -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void mqttSetConfigPrimary(esp_mqtt_client_config_t * mqttCfg)
{
  memset(mqttCfg, 0, sizeof(esp_mqtt_client_config_t));

  // Host
  _mqttData.primary = true;
  #if CONFIG_MQTT1_TYPE == 0
    _mqttData.local = false;
  #else
    _mqttData.local = true;
  #endif // CONFIG_MQTT1_TYPE == 0
  if (_mqttData.host) {
    free(_mqttData.host);
    _mqttData.host = nullptr;
  };
  #if CONFIG_MQTT1_TYPE == 2
    _mqttData.host = wifiGetGatewayIP();
  #else
    _mqttData.host = malloc_string(CONFIG_MQTT1_HOST);
  #endif // CONFIG_MQTT1_TYPE == 2
  mqttCfg->host = _mqttData.host;

  // Port and transport
  #if CONFIG_MQTT1_TLS_ENABLED
    _mqttData.port = CONFIG_MQTT1_PORT_TLS;
    mqttCfg->port = CONFIG_MQTT1_PORT_TLS;
    mqttCfg->skip_cert_common_name_check = false;
    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      mqttCfg->use_tls = true;
    #else 
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
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
  #else
    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      mqttCfg->use_tls = false;
    #else
      mqttCfg->transport = MQTT_TRANSPORT_OVER_TCP;
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
    _mqttData.port = CONFIG_MQTT1_PORT_TCP;
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

  // Connection parameters
  mqttCfg->network_timeout_ms = CONFIG_MQTT1_TIMEOUT;
  mqttCfg->reconnect_timeout_ms = CONFIG_MQTT1_RECONNECT;
  mqttCfg->disable_auto_reconnect = !CONFIG_MQTT1_AUTO_RECONNECT;
  mqttCfg->disable_clean_session = !CONFIG_MQTT1_CLEAN_SESSION;
  #if !defined(CONFIG_MQTT_USE_LWMQTT_CLIENT) || (CONFIG_MQTT_USE_LWMQTT_CLIENT == 0)
    mqttCfg->keepalive = CONFIG_MQTT1_KEEP_ALIVE;
    mqttCfg->disable_keepalive = false;
    mqttCfg->buffer_size = CONFIG_MQTT_READ_BUFFER_SIZE;
    mqttCfg->out_buffer_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
    mqttCfg->task_prio = CONFIG_TASK_PRIORITY_MQTT_CLIENT;
    mqttCfg->task_stack = CONFIG_MQTT_CLIENT_STACK_SIZE;
  #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

  // LWT
  #if CONFIG_MQTT_STATUS_LWT
    mqttCfg->lwt_topic = mqttTopicStatusCreate(true);
    mqttCfg->lwt_msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
    mqttCfg->lwt_msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
    mqttCfg->lwt_qos = CONFIG_MQTT_STATUS_QOS;
    mqttCfg->lwt_retain = CONFIG_MQTT_STATUS_RETAINED;
  #endif // CONFIG_MQTT_STATUS_LWT
}

#ifdef CONFIG_MQTT2_TYPE

void mqttSetConfigReserved(esp_mqtt_client_config_t * mqttCfg)
{
  memset(mqttCfg, 0, sizeof(esp_mqtt_client_config_t));

  // Host
  _mqttData.primary = false;
  #if CONFIG_MQTT2_TYPE == 0
    _mqttData.local = false;
  #else
    _mqttData.local = true;
  #endif // CONFIG_MQTT2_TYPE == 0
  if (_mqttData.host) {
    free(_mqttData.host);
    _mqttData.host = nullptr;
  };
  #if CONFIG_MQTT2_TYPE == 2
    _mqttData.host = wifiGetGatewayIP();
  #else
    _mqttData.host = malloc_string(CONFIG_MQTT2_HOST);
  #endif // CONFIG_MQTT2_TYPE == 2
  mqttCfg->host = _mqttData.host;

  // Port and transport
  #if CONFIG_MQTT2_TLS_ENABLED
    _mqttData.port = CONFIG_MQTT2_PORT_TLS;
    mqttCfg->port = CONFIG_MQTT2_PORT_TLS;
    mqttCfg->skip_cert_common_name_check = false;
    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      mqttCfg->use_tls = true;
    #else 
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
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
  #else
    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      mqttCfg->use_tls = false;
    #else
      mqttCfg->transport = MQTT_TRANSPORT_OVER_TCP;
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
    _mqttData.port = CONFIG_MQTT2_PORT_TCP;
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

  // Connection parameters
  mqttCfg->network_timeout_ms = CONFIG_MQTT2_TIMEOUT;
  mqttCfg->reconnect_timeout_ms = CONFIG_MQTT2_RECONNECT;
  mqttCfg->disable_auto_reconnect = !CONFIG_MQTT2_AUTO_RECONNECT;
  mqttCfg->disable_clean_session = !CONFIG_MQTT2_CLEAN_SESSION;
  #if !defined(CONFIG_MQTT_USE_LWMQTT_CLIENT) || (CONFIG_MQTT_USE_LWMQTT_CLIENT == 0)
    mqttCfg->keepalive = CONFIG_MQTT2_KEEP_ALIVE;
    mqttCfg->disable_keepalive = false;
    mqttCfg->buffer_size = CONFIG_MQTT_READ_BUFFER_SIZE;
    mqttCfg->out_buffer_size = CONFIG_MQTT_WRITE_BUFFER_SIZE;
    mqttCfg->task_prio = CONFIG_TASK_PRIORITY_MQTT_CLIENT;
    mqttCfg->task_stack = CONFIG_MQTT_CLIENT_STACK_SIZE;
  #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

  // LWT
  #if CONFIG_MQTT_STATUS_LWT
    mqttCfg->lwt_topic = mqttTopicStatusCreate(false);
    mqttCfg->lwt_msg = CONFIG_MQTT_STATUS_LWT_PAYLOAD;
    mqttCfg->lwt_msg_len = strlen(CONFIG_MQTT_STATUS_LWT_PAYLOAD);
    mqttCfg->lwt_qos = CONFIG_MQTT_STATUS_QOS;
    mqttCfg->lwt_retain = CONFIG_MQTT_STATUS_RETAINED;
  #endif // CONFIG_MQTT_STATUS_LWT
}

#endif // CONFIG_MQTT2_TYPE

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Task routines -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool mqttClientStart() 
{
  // Disconnect from broker and stop task if it is currently running
  mqttClientStop();

  // Reset variables
  _mqttState = MQTT_CLIENT_STOPPED;
  memset(&_mqttData, 0, sizeof(_mqttData));

  // Configuring broker parameters
  static esp_mqtt_client_config_t _mqttCfg;
  #ifdef CONFIG_MQTT2_TYPE
    if (_mqttPrimary) {
      mqttSetConfigPrimary(&_mqttCfg);
    } else {
      mqttSetConfigReserved(&_mqttCfg);
    };
  #else
    mqttSetConfigPrimary(&_mqttCfg);
  #endif // CONFIG_MQTT2_TYPE

  RE_MEM_CHECK_EVENT(_mqttCfg.host, return false);
  #if CONFIG_MQTT_STATUS_LWT
    RE_MEM_CHECK_EVENT(_mqttCfg.lwt_topic, return false);
  #endif // CONFIG_MQTT_STATUS_LWT
    
  // Launching the client
  #if CONFIG_MQTT_USE_LWMQTT_CLIENT
    // TODO: Need code
  #else
    _mqttClient = esp_mqtt_client_init(&_mqttCfg);
    RE_MEM_CHECK_EVENT(_mqttClient, return false);
    RE_OK_CHECK_EVENT(esp_mqtt_client_register_event(_mqttClient, MQTT_EVENT_ANY, mqttEventHandler, _mqttClient), return false);
    RE_OK_CHECK_EVENT(esp_mqtt_client_start(_mqttClient), return false);
  #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
  
  _mqttState = MQTT_CLIENT_STARTED;
  rlog_i(logTAG, "Task [ MQTT_CLIENT ] was created");
  return true;
}

bool mqttClientDisconnect()
{
  bool ret = false;
  ESP_MQTT_LOCK_MAIN();
  ESP_MQTT_LOCK_SELECT();

  #if CONFIG_MQTT_USE_LWMQTT_CLIENT
    if (_mqttTask) {
      if (_mqttState == MQTT_CLIENT_STARTED) {
        // Attempt to properly disconnect a connected client
        lwmqtt_err_t err = lwmqtt_disconnect(&_mqttClient, _mqttConfig.network_timeout_ms);
        if (err != LWMQTT_SUCCESS) {
          rlog_e(logTAG, "Failed to lwmqtt disconnect: %d", err);
        };

        // Disconnect network
        if (_mqttConfig.use_tls) {
          esp_tls_lwmqtt_network_disconnect(&_mqttTlsNetwork);
        } else {
          esp_lwmqtt_network_disconnect(&_mqttNetwork);
        };

        // Fix state
        _mqttData.connected = false;
        _mqttState = MQTT_CLIENT_SUSPENDED;
        eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_LOST, &_mqttData, sizeof(_mqttData), portMAX_DELAY);

        // Suspend task
        if (eTaskGetState(_mqttTask) != eSuspended) {
          vTaskSuspend(_mqttTask);
          if (eTaskGetState(_mqttTask) == eSuspended) {
            rloga_i("Task [ %s ] has been suspended", mqttTaskName);
            ret = true;
          } else {
            rloga_e("Failed to suspend task [ %s ]!", mqttTaskName);
          };
        };
      } else {
        ret = true;
        rlog_i(logTAG, "Task [ %s ] already stopped", mqttTaskName);
      };
    };
  #else
    if (_mqttClient) {
      if (_mqttState == MQTT_CLIENT_STARTED) {
        // Stop mqtt client
        esp_err_t err = esp_mqtt_client_stop(_mqttClient);
        ret = err == ESP_OK;
        if (ret) {
          _mqttData.connected = false;
          _mqttState = MQTT_CLIENT_SUSPENDED;
          eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_LOST, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
          rlog_i(logTAG, "Task [ MQTT_CLIENT ] was stopped");
        } else {
          mqttErrorEventSendCode("Failed to stop task [ MQTT_CLIENT ]: %d %s", err);
          rlog_e(logTAG, "Failed to stop task [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
        };
      } else {
        ret = true;
        rlog_i(logTAG, "Task [ MQTT_CLIENT ] already stopped");
      };
    };
  #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

  ESP_MQTT_UNLOCK_SELECT();
  ESP_MQTT_UNLOCK_MAIN();
  return ret;
}

bool mqttClientResume()
{
  bool ret = false;
  ESP_MQTT_LOCK_MAIN();
  ESP_MQTT_LOCK_SELECT();

  #if CONFIG_MQTT_USE_LWMQTT_CLIENT
    // TODO: Need code
  #else
    if (_mqttClient) {
      if (_mqttState == MQTT_CLIENT_STARTED) {
        ret = true;
        rlog_w(logTAG, "Task [ MQTT_CLIENT ] already started");
      } else {
        esp_err_t err = esp_mqtt_client_start(_mqttClient);
        ret = err == ESP_OK;
        if (ret) {
          _mqttState = MQTT_CLIENT_STARTED;
          eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_ERROR_CLEAR, nullptr, 0, portMAX_DELAY);
          rlog_i(logTAG, "Task [ MQTT_CLIENT ] was started");
        } else {
          mqttErrorEventSendCode("Failed to start task [ MQTT_CLIENT ]: %d %s", err);
          rlog_e(logTAG, "Failed to start task [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
        };
      };
    } else {
      ret = mqttClientStart();
    };
  #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

  ESP_MQTT_UNLOCK_SELECT();
  ESP_MQTT_UNLOCK_MAIN();
  return ret;
}

bool mqttClientStop()
{
  bool ret = true;

  // Disсonnect from server and stop task
  if (_mqttState == MQTT_CLIENT_STARTED) {
    ret = mqttClientDisconnect();
  };

  if (ret) {
    ESP_MQTT_LOCK_MAIN();
    ESP_MQTT_LOCK_SELECT();

    #if CONFIG_MQTT_USE_LWMQTT_CLIENT
      // TODO: Need code
    #else
      if (_mqttClient) {
        esp_err_t err = esp_mqtt_client_destroy(_mqttClient);
        if (err != ESP_OK) {
          rlog_e(logTAG, "Failed to destroy task [ MQTT_CLIENT ]: %d %s", err, esp_err_to_name(err));
        };

        _mqttClient = nullptr;
        _mqttState = MQTT_CLIENT_STOPPED;
        _mqttData.connected = false;
        _mqttData.conn_attempt = 0;
        eventLoopPost(RE_MQTT_EVENTS, RE_MQTT_CONN_LOST, &_mqttData, sizeof(_mqttData), portMAX_DELAY);
        if (_mqttData.host) {
          free(_mqttData.host);
          _mqttData.host = nullptr;
        };
        #if CONFIG_MQTT_STATUS_LWT || CONFIG_MQTT_STATUS_ONLINE
        mqttTopicStatusFree();
        #endif // CONFIG_MQTT_STATUS_LWT
        rlog_d(logTAG, "Task [ MQTT_CLIENT ] was deleted");
      };
    #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
    
    ESP_MQTT_UNLOCK_SELECT();
    ESP_MQTT_UNLOCK_MAIN();
  };
  return ret;
}

bool mqttTaskInit()
{
  #if CONFIG_MQTT_USE_LWMQTT_CLIENT
    // Create mutexes
    if (_mqttMutexMain == nullptr) {
      #if CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
        _mqttMutexMain = xSemaphoreCreateMutexStatic(&_mqttMutexBufferMain);
      #else
        _mqttMutexMain = xSemaphoreCreateMutex();
      #endif // CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
    };
    RE_MEM_CHECK_EVENT(_mqttMutexMain, return false);
    if (_mqttMutexSelect == nullptr) {
      #if CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
        _mqttMutexSelect = xSemaphoreCreateMutexStatic(&_mqttMutexBufferSelect);
      #else
        _mqttMutexSelect = xSemaphoreCreateMutex();
      #endif // CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
    };
    RE_MEM_CHECK_EVENT(_mqttMutexSelect, return false);

    // Create incoming queue
    if (_mqttIncomingQueue == nullptr) {
      #if CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
        _mqttIncomingQueue = xQueueCreateStatic(CONFIG_MQTT_INCOMING_QUEUE_SIZE, CONFIG_MQTT_INCOMING_EVENT_SIZE, &(_mqttQueueStorage[0]), &_mqttQueueBuffer);
      #else
        _mqttIncomingQueue = xQueueCreate(CONFIG_MQTT_INCOMING_QUEUE_SIZE, CONFIG_MQTT_INCOMING_EVENT_SIZE);
      #endif // CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
    };
    RE_MEM_CHECK_EVENT(_mqttIncomingQueue, return false);

    // Create task
    #if CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
      _mqttTask = xTaskCreateStaticPinnedToCore(mqttTaskExec, mqttTaskName, CONFIG_MQTT_CLIENT_STACK_SIZE, nullptr, CONFIG_TASK_PRIORITY_MQTT_CLIENT, _mqttTaskStack, &_mqttTaskBuffer, CONFIG_TASK_CORE_MQTT_CLIENT); 
    #else
      xTaskCreatePinnedToCore(mqttTaskExec, mqttTaskName, CONFIG_MQTT_CLIENT_STACK_SIZE, nullptr, CONFIG_TASK_PRIORITY_MQTT_CLIENT, &_mqttTask, CONFIG_TASK_CORE_MQTT_CLIENT); 
    #endif // CONFIG_MQTT_CLIENT_STATIC_ALLOCATION
    RE_MEM_CHECK_EVENT(_mqttTask, return false);
  #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT

  return true;
}

bool mqttTaskStart(bool createSuspended)
{
  if (createSuspended) {
    return mqttTaskInit() && mqttEventHandlerRegister();
  } else {
    return mqttTaskInit() && mqttServerSelectAuto();
  };
}

bool mqttTaskFree()
{
  bool ret = mqttClientStop();
  #if CONFIG_MQTT_USE_LWMQTT_CLIENT
    // TODO: Need code
  #endif // CONFIG_MQTT_USE_LWMQTT_CLIENT
  return ret;
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- WiFi event handler -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void mqttWiFiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // STA connected, Internet access not checked
  if (event_id == RE_WIFI_STA_GOT_IP) {
    rlog_v(logTAG, "Event received: RE_WIFI_STA_GOT_IP");
    mqttServerSelectInet(false, true);
  }
  // STA connected and Internet access is available
  else if (event_id == RE_WIFI_STA_PING_OK) {
    rlog_v(logTAG, "Event received: RE_WIFI_STA_PING_OK");
    mqttServerSelectInet(true, true);
  }
  // Internet access lost
  else if (event_id == RE_WIFI_STA_PING_FAILED) {
    rlog_v(logTAG, "Event received: RE_WIFI_STA_PING_FAILED");
    mqttServerSelectInet(false, true);
  }
  // STA disconnected
  else if ((event_id == RE_WIFI_STA_DISCONNECTED) || (event_id == RE_WIFI_STA_STOPPED)) {
    rlog_v(logTAG, "Event received: RE_WIFI_STA_DISCONNECTED");
    if (_mqttState == MQTT_CLIENT_STARTED) {
      mqttClientDisconnect();
    };
  };
}

static void mqttSelfEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if ((event_id == RE_MQTT_SERVER_PRIMARY) || (event_id == RE_MQTT_SERVER_RESERVED)) {
    mqttClientStop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    mqttClientStart();
  };
}

#if defined(CONFIG_MQTT2_TYPE) && defined(CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) && (CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES > 0)
static void mqttTimeEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_TIME_EVERY_MINUTE) {
    _mqttBackToPrimarty++;
    if (_mqttBackToPrimarty >= CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES) {
      _mqttBackToPrimarty = 0;
      rlog_i(logTAG, "Attempting to switch to the primary server...");
      mqttServer1SetAvailable(true);
    };
  };
}
#endif // CONFIG_MQTT_BACK_TO_PRIMARY_TIME_MINUTES

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
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_SERVER_PRIMARY, &mqttSelfEventHandler, nullptr)
      && eventHandlerRegister(RE_MQTT_EVENTS, RE_MQTT_SERVER_RESERVED, &mqttSelfEventHandler, nullptr);
}

