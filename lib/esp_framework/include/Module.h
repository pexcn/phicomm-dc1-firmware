// Module.h

#ifndef _MODULE_h
#define _MODULE_h

#include <ESP8266WebServer.h>
#include "Config.h"
#include "Led.h"
#include "Wifi.h"
#ifndef DISABLE_MQTT
#include "Mqtt.h"
#endif
#include "Debug.h"
#include "Util.h"

class Module
{
public:
    virtual void init();
    virtual String getModuleName();
    virtual String getModuleCNName();
    virtual String getModuleVersion();
    virtual String getModuleAuthor();

    virtual bool moduleLed();

    virtual void loop();
    virtual void perSecondDo();

    virtual void readConfig();
    virtual void resetConfig();
    virtual bool saveConfig();
    virtual bool resetRuntime() { return true; }
    virtual bool flushRuntime() { return true; }

    virtual void httpAdd(ESP8266WebServer *server);
    virtual String httpGetConfig() { return F("{}"); }
    virtual String httpGetStatus(ESP8266WebServer *server);

#ifndef DISABLE_MQTT
    virtual void mqttCallback(char *topic, char *payload, char *cmnd);
    virtual void mqttConnected();
#ifndef DISABLE_MQTT_DISCOVERY
    virtual void mqttDiscovery(bool isEnable = true);
#endif
#endif
};

extern Module *module;
#endif
