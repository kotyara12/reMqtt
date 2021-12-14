# MQTT client for ESP32 / ESP-IDF

MQTT client ESP32 and ESP-IDF (_will not work in Arduino_) with the ability to configure up to two MQTT brokers with automatic switching between them. The broker can be "_local_" (that is, accessible only within the local network to which the ESP is connected), or "_public_" (that is, accessible from the Internet). This allows you to implement a scheme of work when the main MQTT broker is [a local server on a router with a bridge to an external one](https://kotyara12.ru/pubs/iot/keenetic-mqtt/) (or a Home Assistant broker, for example), and as a backup - any public one. In this case, if access from the local network to the Internet is lost, the device continues to function successfully on the local broker, but we lose the ability to be controlled from outside. Conversely, if, for any reason, there is a loss of access to the main broker, the device will automatically switch to the backup server. You can also specify public servers as the primary and backup brokers, simply to increase the reliability of the system. Or use only one server as usual.

The library is based on the ESP-IDF library "mqtt_client.h", so it is unlikely to work on the Arduino framework. In fact, it is just a "wrapper" around "mqtt_client.h" for easier use and automatic server selection. You should note that this library is integrated into a convenient event system, released using other libraries [reEvents](https://github.com/kotyara12/reEvents) and [reStates](https://github.com/kotyara12/reStates). _Full list of dependencies on other libraries see below in the "Dependencies" section_. If you do not plan to use the entire set of libraries presented in my profile, you should fork and modify the code as you wish.

## Usage:

### Create and run an MQTT client task
The ```mqttTaskStart``` function creates and possibly starts an MQTT client task. The ```createSuspended``` parameter allows you to _create a task in suspended form_ in order to _start it later on the event of connecting to a WiFi network_ and (or) gaining access to the Internet. If successful, the function will return true.
```
bool mqttTaskStart(bool createSuspended);
```
### Stopping and deleting an MQTT client task
This function can be called before the device is rebooted. If successful, the function will return true.
```
bool mqttTaskStop();
```

### Suspending and restoring client MQTT task
```
bool mqttTaskSuspend();
bool mqttTaskResume();
```
These functions can be used to suspend a task for a time when the device is disconnected from the WiFi network or access to the server is lost. If successful, the function will return true.

### Registering event handlers
The ```mqttEventHandlerRegister``` function registers the module's event handlers in the main event loop [reEvents](https://github.com/kotyara12/reEvents).
```
bool mqttEventHandlerRegister();
```
With this, the MQTT client will be automatically started or stopped by the events of connecting to the WiFi network. In addition, the MQTT client task itself sends connect or disconnect events to the MQTT broker to notify other firmware subsystems. If successful, the function will return true.

### Getting connection status
Using the ```mqttIsConnected``` function, you can find out whether the client is connected to the broker at the moment or not.
```
bool mqttIsConnected();
```

### Subscribe to topics
**Subscription to a topic** ```topic``` with a specified ```QoS```. If successful, the function will return true.
```
bool mqttSubscribe(const char * topic, int qos);
```

**Canceling a subscription to a previously signed topic** ```topic```. If successful, the function will return true.
```
bool mqttUnsubscribe(const char * topic);
```

### Posting a message to a topic
Publishing the message ```payload``` to the specified topic ```topic``` with the specified parameters ```QoS``` and ```retained```. <br/>
If the ```forced``` bit is set, then an attempt to send a message will be made immediately _in the context of the calling task_; otherwise the message will be queued and sent later _in the context of the client's MQTT task_.<br/>
If ```free_topic = true``` the transferred string ```char * topic``` will be removed from the heap after sending.<br/>
Likewise, if ```free_payload = true``` the transferred string ```char * payload``` will be removed from the heap after sending.<br/>
```
bool mqttPublish(char * topic, char * payload, int qos, bool retained, bool forced, bool free_topic, bool free_payload);
```

## Dependencies:
  - esp_event_base.h (ESP-IDF)
  - mqtt_client.h (ESP-IDF)
  - project_config.h (project settings)
  - https://github.com/kotyara12/rLog
  - https://github.com/kotyara12/rStrings
  - https://github.com/kotyara12/reEsp32
  - https://github.com/kotyara12/reEvents
  - https://github.com/kotyara12/reStates
  - https://github.com/kotyara12/reWifi
  - https://github.com/kotyara12/reNvs

## Notes
These comments refer to my libraries hosted on the resource https://github.com/kotyara12?tab=repositories.

- libraries whose name starts with the **re** prefix are intended only for ESP32 and ESP-IDF (FreeRTOS)
- libraries whose name begins with the **ra** prefix are intended only for ARDUINO
- libraries whose name starts with the **r** prefix can be used for both ARDUINO and ESP-IDF

Since I am currently developing programs mainly for ESP-IDF, the bulk of my libraries are intended only for this framework. But you can port them to another system using.
