#include "esp_mqtt.h"
#include "project_config.h"
#include "rLog.h"
#include "reWiFi.h"
#include "reEsp32.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#if CONFIG_MQTT_TLS_ENABLE
#include "esp_tls_lwmqtt.h"
#endif
#include "esp_lwmqtt.h"
#include "esp_task_wdt.h"

static const char* tagMQTT = "MQTT";
static const char* mqttTaskName = "mqttClient";

static SemaphoreHandle_t esp_mqtt_main_mutex = NULL;
#define ESP_MQTT_LOCK_MAIN() do {} while (xSemaphoreTake(esp_mqtt_main_mutex, portMAX_DELAY) != pdPASS)
#define ESP_MQTT_UNLOCK_MAIN() xSemaphoreGive(esp_mqtt_main_mutex)

static SemaphoreHandle_t esp_mqtt_select_mutex = NULL;
#define ESP_MQTT_LOCK_SELECT() do {} while (xSemaphoreTake(esp_mqtt_select_mutex, portMAX_DELAY) != pdPASS)
#define ESP_MQTT_UNLOCK_SELECT() xSemaphoreGive(esp_mqtt_select_mutex)

static mqtt_status_callback_t esp_mqtt_status_callback = NULL;
static mqtt_message_callback_t esp_mqtt_message_callback = NULL;

static uint8_t esp_mqtt_write_buffer[CONFIG_MQTT_WRITE_BUFFER_SIZE];
static uint8_t esp_mqtt_read_buffer[CONFIG_MQTT_READ_BUFFER_SIZE];

static bool esp_mqtt_running = false;
static bool esp_mqtt_connected = false;
static lwmqtt_err_t esp_mqtt_error = LWMQTT_SUCCESS;

static lwmqtt_client_t esp_mqtt_client;
static esp_lwmqtt_network_t esp_mqtt_network = {0};

#if CONFIG_MQTT_TLS_ENABLE
static bool esp_mqtt_use_tls = false;
static uint32_t esp_mqtt_tls_attempt = 0;
static esp_tls_lwmqtt_network_t esp_mqtt_tls_network = {0};
#endif // CONFIG_MQTT_TLS_ENABLE

#if CONFIG_MQTT_STATUS_LWT
char * esp_mqtt_lwt_topic = NULL;
#endif // CONFIG_MQTT_STATUS_LWT

static esp_lwmqtt_timer_t esp_mqtt_timer_keep_alive, esp_mqtt_timer_command;

typedef struct {
  lwmqtt_string_t topic;
  lwmqtt_message_t message;
} esp_mqtt_event_t;

#define MQTT_CLIENT_QUEUE_ITEM_SIZE sizeof(esp_mqtt_event_t*)
#define MQTT_LOG_PAYLOAD_LIMIT 2048

static TaskHandle_t esp_mqtt_task;
static QueueHandle_t esp_mqtt_event_queue = NULL;

#if CONFIG_MQTT_STATIC_ALLOCATION
StaticSemaphore_t esp_mqtt_main_mutex_buffer;
StaticSemaphore_t esp_mqtt_select_mutex_buffer;
StaticQueue_t esp_mqtt_event_queue_buffer;
uint8_t esp_mqtt_event_queue_storage[CONFIG_MQTT_CLIENT_QUEUE_SIZE * MQTT_CLIENT_QUEUE_ITEM_SIZE];
StaticTask_t esp_mqtt_task_buffer;
StackType_t esp_mqtt_task_stack[CONFIG_MQTT_CLIENT_STACK_SIZE];
#endif // CONFIG_MQTT_STATIC_ALLOCATION

static const char * lwmqtt_err_names[] = {
  "SUCCESS",
  "BUFFER TOO SHORT",
  "VARNUM OVERFLOW",
  "NETWORK FAILED CONNECT",
  "NETWORK TIMEOUT",
  "NETWORK FAILED READ",
  "NETWORK FAILED WRITE",
  "REMAINING LENGTH OVERFLOW",
  "REMAINING LENGTH MISMATCH",
  "MISSING OR WRONG PACKET",
  "CONNECTION DENIED",
  "FAILED SUBSCRIPTION",
  "SUBACK ARRAY OVERFLOW",
  "PONG TIMEOUT"
};

static const char * lwmqtt_return_code_names[] = {
  "CONNECTION ACCEPTED", 
  "IDENTIFIER REJECTED",
  "SERVER UNAVAILABLE",
  "BAD USERNAME OR PASSWORD",
  "NOT AUTHORIZED",
  "UNKNOWN RETURN CODE"
};

bool esp_mqtt_publish_bytes(const char *topic, uint8_t *payload, size_t len, int qos, bool retained);
bool esp_mqtt_publish(const char *topic, const char *payload, int qos, bool retained);

bool esp_mqtt_init(mqtt_status_callback_t scb, mqtt_message_callback_t mcb)
{
  // set callbacks
  esp_mqtt_status_callback = scb;
  esp_mqtt_message_callback = mcb;

  // create mutexes
  #if CONFIG_MQTT_STATIC_ALLOCATION
  esp_mqtt_main_mutex = xSemaphoreCreateMutexStatic(&esp_mqtt_main_mutex_buffer);
  #else
  esp_mqtt_main_mutex = xSemaphoreCreateMutex();
  #endif // CONFIG_MQTT_STATIC_ALLOCATION
  if (!esp_mqtt_main_mutex) {
    rlog_e(tagMQTT, "Can't create main mutex!");
    return false;
  };
  
  #if CONFIG_MQTT_STATIC_ALLOCATION
  esp_mqtt_select_mutex = xSemaphoreCreateMutexStatic(&esp_mqtt_select_mutex_buffer);
  #else
  esp_mqtt_select_mutex = xSemaphoreCreateMutex();
  #endif // CONFIG_MQTT_STATIC_ALLOCATION
  if (!esp_mqtt_select_mutex) {
    vSemaphoreDelete(esp_mqtt_main_mutex);
    rlog_e(tagMQTT, "Can't create select mutex!");
    return false;
  };

  return true;
}

void esp_mqtt_free()
{
  if (esp_mqtt_main_mutex) {
    vSemaphoreDelete(esp_mqtt_main_mutex);
  };
  
  if (esp_mqtt_select_mutex) {
    vSemaphoreDelete(esp_mqtt_select_mutex);
  };
};

#if CONFIG_MQTT_TLS_ENABLE
bool esp_mqtt_init_tls(bool enable, bool verify, const uint8_t *ca_buf, size_t ca_len) 
{
  // acquire mutex
  ESP_MQTT_LOCK_MAIN();

  // disable if requested
  if (!enable) {
    esp_mqtt_use_tls = false;
    esp_mqtt_tls_network.verify = false;
    esp_mqtt_tls_network.ca_buf = NULL;
    esp_mqtt_tls_network.ca_len = 0;
    ESP_MQTT_UNLOCK_MAIN();
    return true;
  }

  // check ca certificate
  if (!ca_buf || ca_len <= 0) {
    rlog_e(tagMQTT, "mqttSetTLS: ca_buf must be not NULL");
    ESP_MQTT_UNLOCK_MAIN();
    return false;
  }

  // set configuration
  esp_mqtt_use_tls = true;
  esp_mqtt_tls_network.verify = verify;
  esp_mqtt_tls_network.ca_buf = (uint8_t *)ca_buf;
  esp_mqtt_tls_network.ca_len = ca_len;

  // release mutex
  ESP_MQTT_UNLOCK_MAIN();

  return true;
}
#endif // CONFIG_MQTT_TLS_ENABLE

#if CONFIG_MQTT_STATUS_LWT
void esp_mqtt_set_lwt_topic(char *topic) 
{
  // acquire mutex
  ESP_MQTT_LOCK_MAIN();

  // set pointers to topic
  esp_mqtt_lwt_topic = topic;

  // release mutex
  ESP_MQTT_UNLOCK_MAIN();
}
#endif // CONFIG_MQTT_STATUS_LWT
 
static void esp_mqtt_message_handler(lwmqtt_client_t *client, void *ref, lwmqtt_string_t topic, lwmqtt_message_t msg) 
{
  // create message
  esp_mqtt_event_t *evt = malloc(sizeof(esp_mqtt_event_t));

  // copy topic with additional null termination
  evt->topic.len = topic.len;
  evt->topic.data = malloc((size_t)topic.len + 1);
  memcpy(evt->topic.data, topic.data, (size_t)topic.len);
  evt->topic.data[topic.len] = 0;

  // copy message with additional null termination
  evt->message.retained = msg.retained;
  evt->message.qos = msg.qos;
  evt->message.payload_len = msg.payload_len;
  evt->message.payload = malloc((size_t)msg.payload_len + 1);
  memcpy(evt->message.payload, msg.payload, (size_t)msg.payload_len);
  evt->message.payload[msg.payload_len] = 0;

  // queue event
  if (xQueueSend(esp_mqtt_event_queue, &evt, 0) != pdTRUE) {
    rlog_e(tagMQTT, "Queue is full, dropping incoming message");
    free(evt->topic.data);
    free(evt->message.payload);
    free(evt);
  };
}

static void esp_mqtt_dispatch_events() 
{
  // prepare event
  esp_mqtt_event_t *evt = NULL;

  // receive next event
  while (xQueueReceive(esp_mqtt_event_queue, &evt, 0) == pdTRUE) {
    // call callback if existing
    if (esp_mqtt_message_callback) {
      esp_mqtt_message_callback(evt->topic.data, evt->message.payload, evt->message.payload_len);
    }

    // free data
    free(evt->topic.data);
    free(evt->message.payload);
    free(evt);
  }
}

static bool esp_mqtt_process_connect() 
{
  // initialize the client
  lwmqtt_init(&esp_mqtt_client, 
    (uint8_t*)&esp_mqtt_write_buffer, CONFIG_MQTT_WRITE_BUFFER_SIZE, 
    (uint8_t*)&esp_mqtt_read_buffer, CONFIG_MQTT_READ_BUFFER_SIZE);

  #if CONFIG_MQTT_TLS_ENABLE
    if (esp_mqtt_use_tls) {
      lwmqtt_set_network(&esp_mqtt_client, &esp_mqtt_tls_network, esp_tls_lwmqtt_network_read, esp_tls_lwmqtt_network_write);
    } else {
      lwmqtt_set_network(&esp_mqtt_client, &esp_mqtt_network, esp_lwmqtt_network_read, esp_lwmqtt_network_write);
    }
  #else
    lwmqtt_set_network(&esp_mqtt_client, &esp_mqtt_network, esp_lwmqtt_network_read, esp_lwmqtt_network_write);
  #endif // CONFIG_MQTT_TLS_ENABLE

  lwmqtt_set_timers(&esp_mqtt_client, &esp_mqtt_timer_keep_alive, &esp_mqtt_timer_command, esp_lwmqtt_timer_set, esp_lwmqtt_timer_get);
  lwmqtt_set_callback(&esp_mqtt_client, NULL, esp_mqtt_message_handler);

  // initiate network connection
  #if CONFIG_MQTT_TLS_ENABLE
    if (esp_mqtt_use_tls) {
      esp_mqtt_error = esp_tls_lwmqtt_network_connect(&esp_mqtt_tls_network, CONFIG_MQTT_HOST, CONFIG_MQTT_PORT_TLS);
    } else {
      esp_mqtt_error = esp_lwmqtt_network_connect(&esp_mqtt_network, CONFIG_MQTT_HOST, CONFIG_MQTT_PORT_TCP);
    }
  #else
    esp_mqtt_error = esp_lwmqtt_network_connect(&esp_mqtt_network, CONFIG_MQTT_HOST, CONFIG_MQTT_PORT_TCP);
  #endif // CONFIG_MQTT_TLS_ENABLE

  if (esp_mqtt_error != LWMQTT_SUCCESS) {
    rlog_e(tagMQTT, "Failed to connect to MQTT server: {network connect} :: #%d ( %s )", 
      esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
    if (esp_mqtt_status_callback) {
      esp_mqtt_status_callback(ESP_MQTT_STATUS_CONNECT_FAILED);
    };
    // if TLS is enabled, try connecting directly
    #if CONFIG_MQTT_TLS_ENABLE
      if (esp_mqtt_use_tls) {
        esp_mqtt_tls_attempt++;
        if (esp_mqtt_tls_attempt > CONFIG_MQTT_TLS_ATTEMPTS) {
          esp_mqtt_use_tls = false;
          if (esp_mqtt_status_callback) {
            esp_mqtt_status_callback(ESP_MQTT_STATUS_TLS_DISABLED);
          };
        };
      };
    #endif // CONFIG_MQTT_TLS_ENABLE
    return false;
  }

  // release main mutex
  ESP_MQTT_UNLOCK_MAIN();

  // acquire select mutex
  ESP_MQTT_LOCK_SELECT();

  // wait for connection
  bool connected = false;

  #if CONFIG_MQTT_TLS_ENABLE
    if (esp_mqtt_use_tls) {
      esp_mqtt_error = esp_tls_lwmqtt_network_wait(&esp_mqtt_tls_network, &connected, CONFIG_MQTT_TIMEOUT);
    } else {
      esp_mqtt_error = esp_lwmqtt_network_wait(&esp_mqtt_network, &connected, CONFIG_MQTT_TIMEOUT);
    }
  #else
    esp_mqtt_error = esp_lwmqtt_network_wait(&esp_mqtt_network, &connected, CONFIG_MQTT_TIMEOUT);
  #endif // CONFIG_MQTT_TLS_ENABLE

  if (esp_mqtt_error != LWMQTT_SUCCESS) {
    rlog_e(tagMQTT, "Failed to connect to MQTT server: {network wait} :: #%d ( %s )", 
      esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
    if (esp_mqtt_status_callback) {
      esp_mqtt_status_callback(ESP_MQTT_STATUS_CONNECT_FAILED);
    };
    return false;
  }

  // release select mutex
  ESP_MQTT_UNLOCK_SELECT();

  // acquire mutex
  ESP_MQTT_LOCK_MAIN();

  // return if not connected
  if (!connected) {
    return false;
  }

  // setup connect data
  lwmqtt_options_t options = lwmqtt_default_options;
  options.client_id = lwmqtt_string(CONFIG_MQTT_CLIENTID);
  options.username = lwmqtt_string(CONFIG_MQTT_USERNAME);
  options.password = lwmqtt_string(CONFIG_MQTT_PASSWORD);
  options.keep_alive = CONFIG_MQTT_KEEP_ALIVE;

  // last will data
  #if CONFIG_MQTT_STATUS_LWT
    lwmqtt_will_t will;
    will.topic = lwmqtt_string(esp_mqtt_lwt_topic);
    will.qos = (lwmqtt_qos_t)CONFIG_MQTT_STATUS_QOS;
    will.retained = CONFIG_MQTT_STATUS_RETAINED;
    will.payload = lwmqtt_string(CONFIG_MQTT_STATUS_LWT_PAYLOAD); 
  #endif // CONFIG_MQTT_LWT_ENABLE

  // attempt connection
  lwmqtt_return_code_t return_code;
  #if CONFIG_MQTT_STATUS_LWT
    esp_mqtt_error = lwmqtt_connect(&esp_mqtt_client, options, &will, &return_code, CONFIG_MQTT_TIMEOUT);
  #else
    esp_mqtt_error = lwmqtt_connect(&esp_mqtt_client, options, NULL, &return_code, CONFIG_MQTT_TIMEOUT);
  #endif // CONFIG_MQTT_LWT_ENABLE
  if (esp_mqtt_error != LWMQTT_SUCCESS) {
    rlog_e(tagMQTT, "Failed to connect to MQTT server: {connect} :: #%d ( %s ) :: #%d ( %s )", 
      esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error],
      return_code, lwmqtt_return_code_names[return_code]);
    if (esp_mqtt_status_callback) {
      esp_mqtt_status_callback(ESP_MQTT_STATUS_CONNECT_FAILED);
    };
    return false;
  }

  #if CONFIG_MQTT_TLS_ENABLE
  esp_mqtt_tls_attempt = 0;
  #endif // CONFIG_MQTT_TLS_ENABLE
  return true;
}

static void esp_mqtt_process_disconnect()
{
  // acquire mutex
  ESP_MQTT_LOCK_MAIN();

  // disconnect lwmqtt
  if (esp_mqtt_connected) {
    lwmqtt_err_t err = lwmqtt_disconnect(&esp_mqtt_client, CONFIG_MQTT_TIMEOUT);
    if (err != LWMQTT_SUCCESS) {
      rlog_w(tagMQTT, "Failed to disconnect from MQTT server: #%d ( %s )", err, lwmqtt_err_names[-1*err]);
    };
    esp_mqtt_connected = false;
  };

  // disconnect network
  #if CONFIG_MQTT_TLS_ENABLE
    if (esp_mqtt_use_tls) {
      esp_tls_lwmqtt_network_disconnect(&esp_mqtt_tls_network);
    } else {
      esp_lwmqtt_network_disconnect(&esp_mqtt_network);
    }
  #else
    esp_lwmqtt_network_disconnect(&esp_mqtt_network);
  #endif // CONFIG_MQTT_TLS_ENABLE

  // set local flags
  esp_mqtt_running = false;

  // release mutex
  ESP_MQTT_UNLOCK_MAIN();

  // show message in log
  rlog_w(tagMQTT, "MQTT server connection closed");

  // call callback if existing
  if (esp_mqtt_status_callback) {
    esp_mqtt_status_callback(ESP_MQTT_STATUS_DISCONNECTED);
  };
}

void esp_mqtt_task_exec(void *pvParameters)
{
  // main loop
  while (true) {
    // set local flag
    esp_mqtt_running = true;
    esp_mqtt_error = LWMQTT_SUCCESS;

    // connection loop
    for (;;) {
      // log attempt
      rlog_i(tagMQTT, "Start connection attempt...");
      // wait WiFi connection
      wifiWaitConnection(portMAX_DELAY);
      // acquire mutex
      ESP_MQTT_LOCK_MAIN();
      // make connection attempt
      if (esp_mqtt_process_connect()) {
        // show message in log
        rlog_i(tagMQTT, "MQTT server connection successful");
        // set local flag
        esp_mqtt_connected = true;
        // release mutex
        ESP_MQTT_UNLOCK_MAIN();
        // exit loop
        break;
      };

      // release mutex
      ESP_MQTT_UNLOCK_MAIN();
      // log fail
      rlog_w(tagMQTT, "Connection attempt failed!");
      // delay loop by 1s and yield to other processes
      vTaskDelay(CONFIG_MQTT_RECONNECT_INTERVAL / portTICK_PERIOD_MS);
    };

    // call callback if existing
    if (esp_mqtt_status_callback) {
      esp_mqtt_status_callback(ESP_MQTT_STATUS_CONNECTED);
    };

    // yield loop
    for (;;) {
      // check for error
      if ((esp_mqtt_error != LWMQTT_SUCCESS) || !esp_mqtt_connected) {
        break;
      };

      vTaskDelay(1);

      // acquire select mutex
      ESP_MQTT_LOCK_SELECT();

      // block until data is available
      bool available = false;

      #if CONFIG_MQTT_TLS_ENABLE
        if (esp_mqtt_use_tls) {
          esp_mqtt_error = esp_tls_lwmqtt_network_select(&esp_mqtt_tls_network, &available, CONFIG_MQTT_TIMEOUT);
        } else {
          esp_mqtt_error = esp_lwmqtt_network_select(&esp_mqtt_network, &available, CONFIG_MQTT_TIMEOUT);
        }
      #else
        esp_mqtt_error = esp_lwmqtt_network_select(&esp_mqtt_network, &available, CONFIG_MQTT_TIMEOUT);
      #endif // CONFIG_MQTT_TLS_ENABLE

      if (esp_mqtt_error != LWMQTT_SUCCESS) {
        rlog_e(tagMQTT, "Failed to lwmqtt network select: #%d ( %s )", 
          esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
        ESP_MQTT_UNLOCK_SELECT();
        if (esp_mqtt_status_callback) {
          esp_mqtt_status_callback(ESP_MQTT_STATUS_ERROR);
        };
        break;
      }

      // release select mutex
      ESP_MQTT_UNLOCK_SELECT();

      // acquire mutex
      ESP_MQTT_LOCK_MAIN();

      // process data if available
      if (available) {
        // get available bytes
        size_t available_bytes = 0;

        #if CONFIG_MQTT_TLS_ENABLE
          if (esp_mqtt_use_tls) {
            esp_mqtt_error = esp_tls_lwmqtt_network_peek(&esp_mqtt_tls_network, &available_bytes, CONFIG_MQTT_TIMEOUT);
          } else {
            esp_mqtt_error = esp_lwmqtt_network_peek(&esp_mqtt_network, &available_bytes);
          }
        #else
          esp_mqtt_error = esp_lwmqtt_network_peek(&esp_mqtt_network, &available_bytes);
        #endif // CONFIG_MQTT_TLS_ENABLE
        if (esp_mqtt_error != LWMQTT_SUCCESS) {
          rlog_e(tagMQTT, "Failed to lwmqtt network peek: #%d ( %s )", 
            esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
          ESP_MQTT_UNLOCK_MAIN();
          if (esp_mqtt_status_callback) {
            esp_mqtt_status_callback(ESP_MQTT_STATUS_ERROR);
          };
          break;
        }

        // yield client only if there is still data to read since select might unblock because of incoming ack packets
        // that are already handled until we get to this point
        if (available_bytes > 0) {
          esp_mqtt_error = lwmqtt_yield(&esp_mqtt_client, available_bytes, CONFIG_MQTT_TIMEOUT);
          if (esp_mqtt_error != LWMQTT_SUCCESS) {
            rlog_e(tagMQTT, "Failed to lwmqtt yield: #%d ( %s )", 
              esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
            ESP_MQTT_UNLOCK_MAIN();
            if (esp_mqtt_status_callback) {
              esp_mqtt_status_callback(ESP_MQTT_STATUS_ERROR);
            };
            break;
          }
        }
      }

      // do mqtt background work
      esp_mqtt_error = lwmqtt_keep_alive(&esp_mqtt_client, CONFIG_MQTT_TIMEOUT);
      if (esp_mqtt_error != LWMQTT_SUCCESS) {
        rlog_e(tagMQTT, "Failed to lwmqtt keep alive: #%d ( %s )", 
          esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
        ESP_MQTT_UNLOCK_MAIN();
        if (esp_mqtt_status_callback) {
          esp_mqtt_status_callback(ESP_MQTT_STATUS_ERROR);
        };
        break;
      }

      // release mutex
      ESP_MQTT_UNLOCK_MAIN();

      // dispatch queued events
      esp_mqtt_dispatch_events();

      esp_task_wdt_reset();
    };

    // close connection
    esp_mqtt_process_disconnect();

    #if CONFIG_MQTT_RECONNECT_AUTO
      vTaskDelay(CONFIG_MQTT_RECONNECT_INTERVAL / portTICK_PERIOD_MS);
    #else
      break;
    #endif // CONFIG_MQTT_RECONNECT_AUTO
  };

  // set local status
  esp_mqtt_running = false;

  // delete task
  vTaskDelete(NULL);
};

bool esp_mqtt_start() 
{
  if (!esp_mqtt_task) {
    ESP_MQTT_LOCK_MAIN();

     // create queue
    if (!esp_mqtt_event_queue) {
      #if CONFIG_MQTT_STATIC_ALLOCATION
      esp_mqtt_event_queue = xQueueCreateStatic(CONFIG_MQTT_CLIENT_QUEUE_SIZE, MQTT_CLIENT_QUEUE_ITEM_SIZE, &(esp_mqtt_event_queue_storage[0]), &esp_mqtt_event_queue_buffer);
      #else
      esp_mqtt_event_queue = xQueueCreate(CONFIG_MQTT_CLIENT_QUEUE_SIZE, MQTT_CLIENT_QUEUE_ITEM_SIZE);
      #endif // CONFIG_MQTT_STATIC_ALLOCATION
      if (!esp_mqtt_event_queue) {
        rloga_e("Failed to create a event queue MQTT client!");
        ESP_MQTT_UNLOCK_MAIN();
        return false;
      };
    };
    
    // start task
    #if CONFIG_MQTT_STATIC_ALLOCATION
    esp_mqtt_task = xTaskCreateStaticPinnedToCore(esp_mqtt_task_exec, mqttTaskName, CONFIG_MQTT_CLIENT_STACK_SIZE, NULL, CONFIG_MQTT_CLIENT_PRIORITY, esp_mqtt_task_stack, &esp_mqtt_task_buffer, CONFIG_MQTT_CLIENT_CORE); 
    #else
    xTaskCreatePinnedToCore(esp_mqtt_task_exec, mqttTaskName, CONFIG_MQTT_CLIENT_STACK_SIZE, NULL, CONFIG_MQTT_CLIENT_PRIORITY, &esp_mqtt_task, CONFIG_MQTT_CLIENT_CORE); 
    #endif // CONFIG_MQTT_STATIC_ALLOCATION
    if (!esp_mqtt_task) {
      vQueueDelete(esp_mqtt_event_queue);
      esp_mqtt_event_queue = NULL;
      rloga_e("Failed to create MQTT client task!");
      ESP_MQTT_UNLOCK_MAIN();
      return false;
    }
    else {
      rloga_d("Task [ %s ] has been successfully started", mqttTaskName);
    };

    ESP_MQTT_UNLOCK_MAIN();
  };
  
  return true;
}

bool esp_mqtt_is_connected()
{
  if (esp_mqtt_main_mutex) {
    // acquire mutex
    ESP_MQTT_LOCK_MAIN();

    // check statue
    if (esp_mqtt_running && esp_mqtt_connected && (esp_mqtt_error == LWMQTT_SUCCESS)) {
      ESP_MQTT_UNLOCK_MAIN();
      return true;
    } else {
      ESP_MQTT_UNLOCK_MAIN();
      return false;
    }
  }
  else {
    return false;
  };
}

int esp_mqtt_last_error()
{
  return (int)esp_mqtt_error;
}

const char * esp_mqtt_last_error_str()
{
  return lwmqtt_err_names[-1*esp_mqtt_error];
}

bool esp_mqtt_publish_bytes(const char *topic, uint8_t *payload, size_t len, int qos, bool retained) 
{
  // acquire mutex
  ESP_MQTT_LOCK_MAIN();
  
  // check if still connected
  if (!esp_mqtt_connected || esp_mqtt_error != LWMQTT_SUCCESS) {
    rlog_e(tagMQTT, "Publish failed: no connection to server!");
    if (esp_mqtt_error == LWMQTT_SUCCESS) {
      esp_mqtt_error = LWMQTT_NETWORK_FAILED_WRITE;
    };
    ESP_MQTT_UNLOCK_MAIN();
    return false;
  }

  // prepare message
  lwmqtt_message_t message;
  message.qos = (lwmqtt_qos_t)qos;
  message.retained = retained;
  message.payload = payload;
  message.payload_len = len;

  // publish message
  esp_mqtt_error = lwmqtt_publish(&esp_mqtt_client, lwmqtt_string(topic), message, CONFIG_MQTT_TIMEOUT);
  if (esp_mqtt_error != LWMQTT_SUCCESS) {
    rlog_e(tagMQTT, "Publish failed: #%d ( %s )", 
      esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
    ESP_MQTT_UNLOCK_MAIN();
    if (esp_mqtt_status_callback) {
      esp_mqtt_status_callback(ESP_MQTT_STATUS_ERROR);
    };
    return false;
  }

  // release mutex
  ESP_MQTT_UNLOCK_MAIN();

  return true;
} 

bool esp_mqtt_subscribe(const char *topic, int qos) {
  // acquire mutex
  ESP_MQTT_LOCK_MAIN();

  // check if still connected
  if (!esp_mqtt_connected) {
    rlog_w(tagMQTT, "Subscribe failed: no connection to server!");
    ESP_MQTT_UNLOCK_MAIN();
    return false;
  }

  // subscribe to topic
  esp_mqtt_error = lwmqtt_subscribe_one(&esp_mqtt_client, lwmqtt_string(topic), (lwmqtt_qos_t)qos, CONFIG_MQTT_TIMEOUT);
  if (esp_mqtt_error == LWMQTT_SUCCESS) {
    rlog_i(tagMQTT, "Subscribed to: %s", topic);
  }
  else {
    rlog_e(tagMQTT, "Subscribe failed: #%d ( %s )", 
      esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
    ESP_MQTT_UNLOCK_MAIN();
    if (esp_mqtt_status_callback) {
      esp_mqtt_status_callback(ESP_MQTT_STATUS_ERROR);
    };
    return false;
  };

  // release mutex
  ESP_MQTT_UNLOCK_MAIN();

  return true;
}

bool esp_mqtt_unsubscribe(const char *topic) {
  // acquire mutex
  ESP_MQTT_LOCK_MAIN();

  // check if still connected
  if (!esp_mqtt_connected) {
    rlog_w(tagMQTT, "Unsubscribe failed: no connection to server!");
    ESP_MQTT_UNLOCK_MAIN();
    return false;
  }

  // unsubscribe from topic
  esp_mqtt_error = lwmqtt_unsubscribe_one(&esp_mqtt_client, lwmqtt_string(topic), CONFIG_MQTT_TIMEOUT);
  if (esp_mqtt_error == LWMQTT_SUCCESS) {
    rlog_i(tagMQTT, "Unsubscribed from: %s", topic);
  }
  else {
    rlog_e(tagMQTT, "Unsubscribe failed: #%d ( %s )", 
      esp_mqtt_error, lwmqtt_err_names[-1*esp_mqtt_error]);
    ESP_MQTT_UNLOCK_MAIN();
    if (esp_mqtt_status_callback) {
      esp_mqtt_status_callback(ESP_MQTT_STATUS_ERROR);
    };
    return false;
  };

  // release mutex
  ESP_MQTT_UNLOCK_MAIN();

  return true;
} 

bool esp_mqtt_publish(const char *topic, const char *payload, int qos, bool retained)
{
  size_t len = strlen(payload);
  bool ret = esp_mqtt_publish_bytes(topic, (uint8_t*)payload, len, qos, retained);
  if (ret) {
    if (len > MQTT_LOG_PAYLOAD_LIMIT) {
      rlog_i(tagMQTT, "Published: %s [ %d bytes ]", topic, len);
    } else {
      rlog_i(tagMQTT, "Published: %s [ %s ]", topic, payload);
    };
  };
  return ret;
}

void esp_mqtt_stop()
{
  if (esp_mqtt_task) {
    // close connection
    esp_mqtt_process_disconnect();

    // acquire mutexes
    ESP_MQTT_LOCK_MAIN();
    ESP_MQTT_LOCK_SELECT();

    // delete queues
    if (esp_mqtt_event_queue) {
      vQueueDelete(esp_mqtt_event_queue);
      esp_mqtt_event_queue = NULL;
    };

    // kill mqtt task
    vTaskDelete(esp_mqtt_task);
    esp_mqtt_task = NULL;
    rloga_d("Task [ %s ] was deleted", mqttTaskName);

    // release mutexes
    ESP_MQTT_UNLOCK_SELECT();
    ESP_MQTT_UNLOCK_MAIN();
  };
}


