#include "Framework.h"
#include "Module.h"
#include "Rtc.h"
#include "Http.h"
#include "Util.h"

uint16_t Framework::rebootCount = 0;
#ifndef DISABLE_MQTT
void Framework::callback(char *topic, byte *payload, unsigned int length)
{
    Debug::AddInfo(PSTR("MQTT  Command %s: %.*s"), topic, length, payload);

    // PubSubClient 2.8 shifts the topic over its length field before callback.
    // Reuse the preceding byte so even a full receive buffer can end in NUL.
    memmove(topic - 1, topic, payload + length - (byte *)topic);
    --topic;
    --payload;
    payload[length] = 0;

    char *cmnd = strrchr(topic, '/');
    if (cmnd == nullptr)
    {
        return;
    }
    cmnd++;

    if (strcmp(cmnd, "restart") == 0)
    {
        if (!Config::prepareRestart()) return;
        ESP.reset();
    }
    else if (module)
    {
        module->mqttCallback(topic, (char *)payload, cmnd);
    }

    Led::led(200);
}

void Framework::connectedCallback()
{
    Mqtt::subscribe(Mqtt::getCmndTopic(F("#")));
    Led::blinkLED(40, 8);
    if (module)
    {
        module->mqttConnected();
    }
}
#endif

void Framework::tickerPerSecondDo()
{
    perSecond++;
    if (perSecond == 30)
    {
        Rtc::rtcReboot.fast_reboot_count = 0;
        Rtc::rtcRebootSave();
    }
    if (rebootCount == 3)
    {
        return;
    }
    Rtc::perSecondDo();

#ifndef DISABLE_MQTT
    Mqtt::perSecondDo();
#endif
    module->perSecondDo();
}

void Framework::one(unsigned long baud)
{
    Rtc::rtcRebootLoad();
    Rtc::rtcReboot.fast_reboot_count++;
    Rtc::rtcRebootSave();
    rebootCount = Rtc::rtcReboot.fast_reboot_count > BOOT_LOOP_OFFSET ? Rtc::rtcReboot.fast_reboot_count - BOOT_LOOP_OFFSET : 0;

    Serial.begin(baud);
    globalConfig.debug.type = 1;
}

void Framework::setup()
{
    if (rebootCount == 1)
    {
        Config::readConfig();
        module->resetConfig();
    }
    else if (rebootCount == 2)
    {
        Config::readConfig();
        module->resetConfig();
    }
    else
    {
        Config::readConfig();
    }
    if (globalConfig.uid[0] != '\0')
    {
        strlcpy(UID, globalConfig.uid, sizeof(UID));
    }
    else
    {
        uint8_t mac[6];
        wifi_get_macaddr(STATION_IF, mac);
        snprintf_P(UID, sizeof(UID), PSTR("%.*s_%02x%02x%02x"),
            int(sizeof(UID) - 8), module->getModuleName().c_str(),
            unsigned(mac[3]), unsigned(mac[4]), unsigned(mac[5]));
    }
    Util::strlowr(UID);

    Debug::AddInfo(PSTR("SYS   Firmware %s, device=%s"), module->getModuleVersion().c_str(), UID);

    if (MQTT_MAX_PACKET_SIZE == 128)
    {
        Debug::AddError(PSTR("MQTT  Packet buffer too small; check PubSubClient build flags"));
    }

    if (rebootCount == 3)
    {
        module = NULL;

        tickerPerSecond = new Ticker();
        tickerPerSecond->attach(1, tickerPerSecondDo);

        Http::begin();
        Wifi::connectWifi();
    }
    else
    {
#ifndef DISABLE_MQTT
        Mqtt::setClient(Wifi::wifiClient);
        Mqtt::mqttSetConnectedCallback(connectedCallback);
        Mqtt::mqttSetLoopCallback(callback);
#endif
        module->init();
        tickerPerSecond = new Ticker();
        tickerPerSecond->attach(1, tickerPerSecondDo);
        Http::begin();
        Wifi::connectWifi();
        Rtc::init();
    }
}

void Framework::loop()
{
    if (rebootCount == 3)
    {
        Wifi::loop();
        Http::loop();
    }
    else
    {
        yield();
        Led::loop();
#ifndef DISABLE_MQTT
        yield();
        Mqtt::loop();
#endif
        yield();
        module->loop();
        Config::loop();
        yield();
        Wifi::loop();
        yield();
        Http::loop();
        yield();
        Rtc::loop();
    }
}
