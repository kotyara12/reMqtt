/* 
   EN: MQTT client for ESP32 (ESP-IDF), based on esp-mqtt by Joël Gähwiler (https://github.com/256dpi/esp-mqtt)
   RU: MQTT клиент для ESP32 (ESP-IDF), основан на esp-mqtt от Joël Gähwiler (https://github.com/256dpi/esp-mqtt)
   --------------------------
   (с) 2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __ESP_MQTT_H__
#define __ESP_MQTT_H__

#include <stddef.h>
#include <sys/types.h>
#include <lwmqtt.h>

/**
 * The statuses emitted by the status callback.
 */
typedef enum { 
  ESP_MQTT_STATUS_DISCONNECTED = 0, 
  ESP_MQTT_STATUS_CONNECTED = 1,
  ESP_MQTT_STATUS_CONNECT_FAILED = 2,
  ESP_MQTT_STATUS_TLS_DISABLED = 3,
  ESP_MQTT_STATUS_ERROR = 4
} esp_mqtt_status_t;

/**
 * The status callback.
 */
typedef void (*mqtt_status_callback_t) (esp_mqtt_status_t); 

/**
 * The message callback.
 */
typedef void (*mqtt_message_callback_t) (char *topic, uint8_t *payload, size_t len);

/**
 * Initialize the MQTT management system.
 * Note: Should only be called once on boot.
 *
 * @param scb - The status callback.
 * @param mcb - The message callback.
 * @return Whether initialize was successful.
 */
bool esp_mqtt_init(mqtt_status_callback_t scb, mqtt_message_callback_t mcb);

/**
 * Deinitialize the MQTT management system.
 */
void esp_mqtt_free();

#if CONFIG_MQTT_TLS_ENABLE
/**
 * Configure TLS connection.
 * The specified CA certificate is not copied and must be available during the whole duration of the MQTT usage.
 * Note: This method must be called before `esp_mqtt_start`.
 *
 * @param enable - Whether TLS should be used.
 * @param verify - Whether the connection should be verified.
 * @param ca_buf - The beginning of the CA certificate buffer.
 * @param ca_len - The length of the CA certificate buffer.
 * @return Whether TLS configuration was successful.
 */
bool esp_mqtt_init_tls(bool enable, bool verify, const uint8_t *ca_buf, size_t ca_len);
#endif // CONFIG_MQTT_TLS_ENABLE

/**
 * Configure Last Will and Testament topic.
 * 
 * Only the topic itself is configured (since it is generated dynamically), 
 * the rest of the parameters are taken directly from the `project_config.h`
 *
 * Note: Must be called before `esp_mqtt_start`.
 *
 * @param topic - A pointer to the LWT topic.
 */
#if CONFIG_MQTT_STATUS_LWT
void esp_mqtt_set_lwt_topic(char *topic);
#endif // CONFIG_MQTT_STATUS_LWT

/**
 * Start the MQTT process.
 *
 * The background process will attempt to connect to the specified broker once a second until a connection can be
 * established. This process can be interrupted by calling `esp_mqtt_stop();`. If a connection has been established,
 * the status callback will be called with `ESP_MQTT_STATUS_CONNECTED`. From that moment on the functions
 * `esp_mqtt_subscribe`, `esp_mqtt_unsubscribe` and `esp_mqtt_publish` can be used to interact with the broker.
 *
 * @return Whether the operation was successful.
 */
bool esp_mqtt_start();

/**
 * Check connection status.
 * 
 * @return Is the connection working at the moment.
 */
bool esp_mqtt_is_connected();

/**
 * Returns the error code for the last operation
 * 
 * @return Error code for the last operation
 */
int esp_mqtt_last_error();

/**
 * Returns the error text for the last operation
 * 
 * @return Error text for the last operation
 */
const char * esp_mqtt_last_error_str();

/**
 * Publish bytes payload to specified topic.
 *
 * When false is returned the current operation failed and any subsequent interactions will also fail. This can be used
 * to handle errors early. As soon as the background process unblocks the error will be detected, the connection closed
 * and the status callback invoked with `ESP_MQTT_STATUS_DISCONNECTED`. After that, an attempt will be made to 
 * automatically reconnect to the server.
 *
 * @param topic - The topic.
 * @param payload - The payload.
 * @param len - The payload length.
 * @param qos - The qos level.
 * @param retained - The retained flag.
 * @return Whether the operation was successful.
 */
bool esp_mqtt_publish_bytes(const char *topic, uint8_t *payload, size_t len, int qos, bool retained); 

/**
 * Subscribe to specified topic.
 *
 * When false is returned the current operation failed and any subsequent interactions will also fail. This can be used
 * to handle errors early. As soon as the background process unblocks the error will be detected, the connection closed
 * and the status callback invoked with `ESP_MQTT_STATUS_DISCONNECTED`. That callback then can simply call
 * `esp_mqtt_start()` to attempt an reconnection.
 *
 * @param topic - The topic.
 * @param qos - The qos level.
 * @return Whether the operation was successful.
 */
bool esp_mqtt_subscribe(const char *topic, int qos);

/**
 * Unsubscribe from specified topic.
 *
 * When false is returned the current operation failed and any subsequent interactions will also fail. This can be used
 * to handle errors early. As soon as the background process unblocks the error will be detected, the connection closed
 * and the status callback invoked with `ESP_MQTT_STATUS_DISCONNECTED`. That callback then can simply call
 * `esp_mqtt_start()` to attempt an reconnection.
 *
 * @param topic - The topic.
 * @return Whether the operation was successful.
 */
bool esp_mqtt_unsubscribe(const char *topic); 

/**
 * Publish string payload to specified topic.
 *
 * When false is returned the current operation failed and any subsequent interactions will also fail. This can be used
 * to handle errors early. As soon as the background process unblocks the error will be detected, the connection closed
 * and the status callback invoked with `ESP_MQTT_STATUS_DISCONNECTED`. After that, an attempt will be made to 
 * automatically reconnect to the server.
 *
 * @param topic - The topic.
 * @param payload - The payload.
 * @param qos - The qos level.
 * @param retained - The retained flag.
 * @return Whether the operation was successful.
 */
bool esp_mqtt_publish(const char *topic, const char *payload, int qos, bool retained); 

/**
 * Stop the MQTT process.
 * Will stop initial connection attempts or disconnect any active connection.
 */
void esp_mqtt_stop();

#endif // __ESP_MQTT_H__
