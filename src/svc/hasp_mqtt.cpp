/* MIT License - Copyright (c) 2020 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

#include "hasp_conf.h"
#if HASP_USE_MQTT > 0

    #include "PubSubClient.h"

    #include "hasp/hasp.h"
    #include "hasp_mqtt.h"
    #include "hasp_mqtt_ha.h"

    #if defined(ARDUINO_ARCH_ESP32)
        #include <WiFi.h>
WiFiClient mqttNetworkClient;
    #elif defined(ARDUINO_ARCH_ESP8266)
        #include <ESP8266WiFi.h>
        #include <EEPROM.h>
        #include <Esp.h>
WiFiClient mqttNetworkClient;
    #else
        #if defined(STM32F4xx) && HASP_USE_WIFI > 0
// #include <WiFi.h>
WiFiSpiClient mqttNetworkClient;
        #else
            #if defined(W5500_MOSI) && defined(W5500_MISO) && defined(W5500_SCLK)
                #define W5500_LAN
                #include <Ethernet.h>
            #else
                #include <STM32Ethernet.h>
            #endif

EthernetClient mqttNetworkClient;
        #endif
    #endif

    #include "hasp_hal.h"
    #include "hasp_debug.h"
    #include "hasp_config.h"

    #include "../hasp/hasp_dispatch.h"

    #ifdef USE_CONFIG_OVERRIDE
        #include "user_config_override.h"
    #endif

char mqttNodeTopic[24];
char mqttGroupTopic[24];
bool mqttEnabled        = false;
bool mqttHAautodiscover = true;

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    // These defaults may be overwritten with values saved by the web interface
    #ifndef MQTT_HOST
        #define MQTT_HOST "";
    #endif

    #ifndef MQTT_PORT
        #define MQTT_PORT 1883;
    #endif

    #ifndef MQTT_USER
        #define MQTT_USER "";
    #endif

    #ifndef MQTT_PASSW
        #define MQTT_PASSW "";
    #endif
    #ifndef MQTT_NODENAME
        #define MQTT_NODENAME "";
    #endif
    #ifndef MQTT_GROUPNAME
        #define MQTT_GROUPNAME "";
    #endif

    #ifndef MQTT_PREFIX
        #define MQTT_PREFIX "hasp"
    #endif

    #define LWT_TOPIC "LWT"

char mqttServer[16]    = MQTT_HOST;
char mqttUser[23]      = MQTT_USER;
char mqttPassword[32]  = MQTT_PASSW;
char mqttNodeName[16]  = MQTT_NODENAME;
char mqttGroupName[16] = MQTT_GROUPNAME;
uint16_t mqttPort      = MQTT_PORT;
PubSubClient mqttClient(mqttNetworkClient);

static bool mqttPublish(const char * topic, const char * payload, size_t len, bool retain = false)
{
    if(mqttIsConnected()) {
        if(mqttClient.beginPublish(topic, len, retain)) {
            mqttClient.write((uint8_t *)payload, len);
            mqttClient.endPublish();

            LOG_TRACE(TAG_MQTT_PUB, F("%s => %s"), topic, payload);
            return true;
        } else {
            LOG_ERROR(TAG_MQTT_PUB, F(D_MQTT_FAILED " %s => %s"), topic, payload);
        }
    } else {
        LOG_ERROR(TAG_MQTT, F(D_MQTT_NOT_CONNECTED));
    }
    return false;
}

static bool mqttPublish(const char * topic, const char * payload, bool retain = false)
{
    return mqttPublish(topic, payload, strlen(payload), retain);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Send changed values OUT

bool mqttIsConnected()
{
    return mqttEnabled && mqttClient.connected();
}

void mqtt_send_lwt(bool online)
{
    char tmp_payload[8];
    char tmp_topic[strlen(mqttNodeTopic) + 4];
    strncpy(tmp_topic, mqttNodeTopic, sizeof(tmp_topic));
    strncat_P(tmp_topic, PSTR(LWT_TOPIC), sizeof(tmp_topic));
    // snprintf_P(tmp_topic, sizeof(tmp_topic), PSTR("%s" LWT_TOPIC), mqttNodeTopic);

    size_t len = snprintf_P(tmp_payload, sizeof(tmp_payload), online ? PSTR("online") : PSTR("offline"));
    bool res   = mqttPublish(tmp_topic, tmp_payload, len, true);
}

void mqtt_send_object_state(uint8_t pageid, uint8_t btnid, char * payload)
{
    char tmp_topic[strlen(mqttNodeTopic) + 16];
    snprintf_P(tmp_topic, sizeof(tmp_topic), PSTR("%sstate/" HASP_OBJECT_NOTATION), mqttNodeTopic, pageid, btnid);
    mqttPublish(tmp_topic, payload);
}

void mqtt_send_state(const __FlashStringHelper * subtopic, const char * payload)
{
    char tmp_topic[strlen(mqttNodeTopic) + 20];
    snprintf_P(tmp_topic, sizeof(tmp_topic), PSTR("%sstate/%s"), mqttNodeTopic, subtopic);
    mqttPublish(tmp_topic, payload);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Receive incoming messages
static void mqtt_message_cb(char * topic, byte * payload, unsigned int length)
{ // Handle incoming commands from MQTT
    if(length + 1 >= mqttClient.getBufferSize()) {
        LOG_ERROR(TAG_MQTT_RCV, F("Payload too long (%d bytes)"), length);
        return;
    } else {
        payload[length] = '\0';
    }

    LOG_TRACE(TAG_MQTT_RCV, F("%s = %s"), topic, (char *)payload);

    if(topic == strstr(topic, mqttNodeTopic)) { // startsWith mqttNodeTopic

        // Node topic
        topic += strlen(mqttNodeTopic); // shorten topic

    } else if(topic == strstr(topic, mqttGroupTopic)) { // startsWith mqttGroupTopic

        // Group topic
        topic += strlen(mqttGroupTopic); // shorten topic
        dispatch_topic_payload(topic, (const char *)payload);
        return;

    } else if(topic == strstr_P(topic, PSTR("homeassistant/status"))) { // HA discovery topic
        if(mqttHAautodiscover && !strcasecmp_P((char *)payload, PSTR("online"))) {
            dispatch_current_state();
            mqtt_ha_register_auto_discovery();
        }
        return;

    } else {
        // Other topic
        LOG_ERROR(TAG_MQTT, F(D_MQTT_INVALID_TOPIC));
        return;
    }

    // catch a dangling LWT from a previous connection if it appears
    if(!strcmp_P(topic, PSTR(LWT_TOPIC))) { // endsWith LWT
        if(!strcasecmp_P((char *)payload, PSTR("offline"))) {
            {
                char msg[8];
                char tmp_topic[strlen(mqttNodeTopic) + 8];
                snprintf_P(tmp_topic, sizeof(tmp_topic), PSTR("%s" LWT_TOPIC), mqttNodeTopic);
                snprintf_P(msg, sizeof(msg), PSTR("online"));

                /*bool res =*/mqttClient.publish(tmp_topic, msg, true);
            }

        } else {
            // LOG_TRACE(TAG_MQTT, F("ignoring LWT = online"));
        }
    } else {
        dispatch_topic_payload(topic, (const char *)payload);
    }
}

static void mqttSubscribeTo(const __FlashStringHelper * format, const char * data)
{
    char tmp_topic[strlen_P((PGM_P)format) + 2 + strlen(data)];
    snprintf_P(tmp_topic, sizeof(tmp_topic), (PGM_P)format, data);
    if(mqttClient.subscribe(tmp_topic)) {
        LOG_VERBOSE(TAG_MQTT, F(D_BULLET D_MQTT_SUBSCRIBED), tmp_topic);
    } else {
        LOG_ERROR(TAG_MQTT, F(D_MQTT_NOT_SUBSCRIBED), tmp_topic);
    }
}

void mqttStart()
{
    char buffer[64];
    char mqttClientId[64];
    char lastWillPayload[8];
    static uint8_t mqttReconnectCount = 0;
    //   bool mqttFirstConnect             = true;

    mqttClient.setServer(mqttServer, 1883);
    // mqttClient.setSocketTimeout(10); //in seconds

    /* Construct unique Client ID*/
    {
        String mac = halGetMacAddress(3, "");
        mac.toLowerCase();
        memset(mqttClientId, 0, sizeof(mqttClientId));
        snprintf_P(mqttClientId, sizeof(mqttClientId), PSTR(D_MQTT_DEFAULT_NAME), mac.c_str());
        LOG_INFO(TAG_MQTT, mqttClientId);
    }

    // Attempt to connect and set LWT and Clean Session
    snprintf_P(buffer, sizeof(buffer), PSTR("%s" LWT_TOPIC), mqttNodeTopic); // lastWillTopic
    snprintf_P(lastWillPayload, sizeof(lastWillPayload), PSTR("offline"));   // lastWillPayload

    haspProgressMsg(F(D_MQTT_CONNECTING));
    haspProgressVal(mqttReconnectCount * 5);
    if(!mqttClient.connect(mqttClientId, mqttUser, mqttPassword, buffer, 0, true, lastWillPayload, true)) {
        // Retry until we give up and restart after connectTimeout seconds
        mqttReconnectCount++;

        switch(mqttClient.state()) {
            case MQTT_CONNECTION_TIMEOUT:
                LOG_WARNING(TAG_MQTT, F("Connection timeout"));
                break;
            case MQTT_CONNECTION_LOST:
                LOG_WARNING(TAG_MQTT, F("Connection lost"));
                break;
            case MQTT_CONNECT_FAILED:
                LOG_WARNING(TAG_MQTT, F("Connection failed"));
                break;
            case MQTT_DISCONNECTED:
                snprintf_P(buffer, sizeof(buffer), PSTR(D_MQTT_DISCONNECTED));
                break;
            case MQTT_CONNECTED:
                break;
            case MQTT_CONNECT_BAD_PROTOCOL:
                LOG_WARNING(TAG_MQTT, F("MQTT version not suported"));
                break;
            case MQTT_CONNECT_BAD_CLIENT_ID:
                LOG_WARNING(TAG_MQTT, F("Client ID rejected"));
                break;
            case MQTT_CONNECT_UNAVAILABLE:
                LOG_WARNING(TAG_MQTT, F("Server unavailable"));
                break;
            case MQTT_CONNECT_BAD_CREDENTIALS:
                LOG_WARNING(TAG_MQTT, F("Bad credentials"));
                break;
            case MQTT_CONNECT_UNAUTHORIZED:
                LOG_WARNING(TAG_MQTT, F("Unauthorized"));
                break;
            default:
                LOG_WARNING(TAG_MQTT, F("Unknown failure"));
        }

        if(mqttReconnectCount > 20) {
            LOG_ERROR(TAG_MQTT, F("Retry count exceeded, rebooting..."));
            dispatch_reboot(false);
        }
        return;
    }

    LOG_INFO(TAG_MQTT, F(D_MQTT_CONNECTED), mqttServer, mqttClientId);

    // Subscribe to our incoming topics
    const __FlashStringHelper * F_topic;
    F_topic = F("%scommand/#");
    mqttSubscribeTo(F_topic, mqttGroupTopic);
    mqttSubscribeTo(F_topic, mqttNodeTopic);
    F_topic = F("%sconfig/#");
    mqttSubscribeTo(F_topic, mqttGroupTopic);
    mqttSubscribeTo(F_topic, mqttNodeTopic);
    mqttSubscribeTo(F("%slight/#"), mqttNodeTopic);
    mqttSubscribeTo(F("%sbrightness/#"), mqttNodeTopic);
    // mqttSubscribeTo(F("%s"LWT_TOPIC), mqttNodeTopic);
    mqttSubscribeTo(F("hass/status"), mqttClientId);

    /* Home Assistant auto-configuration */
    if(mqttHAautodiscover) mqttSubscribeTo(F("homeassistant/status"), mqttClientId);

    // Force any subscribed clients to toggle offline/online when we first connect to
    // make sure we get a full panel refresh at power on.  Sending offline,
    // "online" will be sent by the mqttStatusTopic subscription action.
    mqtt_send_lwt(true);

    // mqttFirstConnect   = false;
    mqttReconnectCount = 0;

    haspReconnect();
    haspProgressVal(255);

    dispatch_current_state();
}

void mqttSetup()
{
    mqttEnabled = strlen(mqttServer) > 0 && mqttPort > 0;
    if(mqttEnabled) {
        mqttClient.setServer(mqttServer, mqttPort);
        mqttClient.setCallback(mqtt_message_cb);
        //  if(!mqttClient.setBufferSize(1024)) {
        //  LOG_ERROR(TAG_MQTT, F("Buffer allocation failed"));
        //  } else {
        LOG_INFO(TAG_MQTT, F(D_MQTT_STARTED), mqttClient.getBufferSize());
        // }
    } else {
        LOG_WARNING(TAG_MQTT, F(D_MQTT_NOT_CONFIGURED));
    }
}

void mqttLoop(void)
{
    if(mqttEnabled) mqttClient.loop();
}

void mqttEvery5Seconds(bool networkIsConnected)
{
    if(mqttEnabled && networkIsConnected && !mqttClient.connected()) {
        LOG_TRACE(TAG_MQTT, F(D_MQTT_RECONNECTING));
        mqttStart();
    }
}

String mqttGetNodename()
{
    return mqttNodeName;
}

void mqttStop()
{
    if(mqttEnabled && mqttClient.connected()) {
        LOG_TRACE(TAG_MQTT, F(D_MQTT_DISCONNECTING));
        mqtt_send_lwt(false);
        mqttClient.disconnect();
        LOG_INFO(TAG_MQTT, F(D_MQTT_DISCONNECTED));
    }
}

    #if HASP_USE_CONFIG > 0
bool mqttGetConfig(const JsonObject & settings)
{
    bool changed = false;

    if(strcmp(mqttNodeName, settings[FPSTR(FP_CONFIG_NAME)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_NAME)] = mqttNodeName;

    if(strcmp(mqttGroupName, settings[FPSTR(FP_CONFIG_GROUP)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_GROUP)] = mqttGroupName;

    if(strcmp(mqttServer, settings[FPSTR(FP_CONFIG_HOST)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_HOST)] = mqttServer;

    if(mqttPort != settings[FPSTR(FP_CONFIG_PORT)].as<uint16_t>()) changed = true;
    settings[FPSTR(FP_CONFIG_PORT)] = mqttPort;

    if(strcmp(mqttUser, settings[FPSTR(FP_CONFIG_USER)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_USER)] = mqttUser;

    if(strcmp(mqttPassword, settings[FPSTR(FP_CONFIG_PASS)].as<String>().c_str()) != 0) changed = true;
    settings[FPSTR(FP_CONFIG_PASS)] = mqttPassword;

    if(changed) configOutput(settings, TAG_MQTT);
    return changed;
}

/** Set MQTT Configuration.
 *
 * Read the settings from json and sets the application variables.
 *
 * @note: data pixel should be formated to uint32_t RGBA. Imagemagick requirements.
 *
 * @param[in] settings    JsonObject with the config settings.
 **/
bool mqttSetConfig(const JsonObject & settings)
{
    configOutput(settings, TAG_MQTT);
    bool changed = false;

    changed |= configSet(mqttPort, settings[FPSTR(FP_CONFIG_PORT)], F("mqttPort"));

    if(!settings[FPSTR(FP_CONFIG_NAME)].isNull()) {
        changed |= strcmp(mqttNodeName, settings[FPSTR(FP_CONFIG_NAME)]) != 0;
        strncpy(mqttNodeName, settings[FPSTR(FP_CONFIG_NAME)], sizeof(mqttNodeName));
    }
    // Prefill node name
    if(strlen(mqttNodeName) == 0) {
        String mac = halGetMacAddress(3, "");
        mac.toLowerCase();
        snprintf_P(mqttNodeName, sizeof(mqttNodeName), PSTR(D_MQTT_DEFAULT_NAME), mac.c_str());
        changed = true;
    }

    if(!settings[FPSTR(FP_CONFIG_GROUP)].isNull()) {
        changed |= strcmp(mqttGroupName, settings[FPSTR(FP_CONFIG_GROUP)]) != 0;
        strncpy(mqttGroupName, settings[FPSTR(FP_CONFIG_GROUP)], sizeof(mqttGroupName));
    }

    if(strlen(mqttGroupName) == 0) {
        strcpy_P(mqttGroupName, PSTR("plates"));
        changed = true;
    }

    if(!settings[FPSTR(FP_CONFIG_HOST)].isNull()) {
        changed |= strcmp(mqttServer, settings[FPSTR(FP_CONFIG_HOST)]) != 0;
        strncpy(mqttServer, settings[FPSTR(FP_CONFIG_HOST)], sizeof(mqttServer));
    }

    if(!settings[FPSTR(FP_CONFIG_USER)].isNull()) {
        changed |= strcmp(mqttUser, settings[FPSTR(FP_CONFIG_USER)]) != 0;
        strncpy(mqttUser, settings[FPSTR(FP_CONFIG_USER)], sizeof(mqttUser));
    }

    if(!settings[FPSTR(FP_CONFIG_PASS)].isNull() &&
       settings[FPSTR(FP_CONFIG_PASS)].as<String>() != String(FPSTR(D_PASSWORD_MASK))) {
        changed |= strcmp(mqttPassword, settings[FPSTR(FP_CONFIG_PASS)]) != 0;
        strncpy(mqttPassword, settings[FPSTR(FP_CONFIG_PASS)], sizeof(mqttPassword));
    }

    snprintf_P(mqttNodeTopic, sizeof(mqttNodeTopic), PSTR(MQTT_PREFIX "/%s/"), mqttNodeName);
    snprintf_P(mqttGroupTopic, sizeof(mqttGroupTopic), PSTR(MQTT_PREFIX "/%s/"), mqttGroupName);

    return changed;
}
    #endif // HASP_USE_CONFIG

#endif // HASP_USE_MQTT
