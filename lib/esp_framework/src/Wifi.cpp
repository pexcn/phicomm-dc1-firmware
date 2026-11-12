#include <WiFiClient.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include "Wifi.h"
#include "Debug.h"

WiFiClient Wifi::wifiClient;
WiFiEventHandler Wifi::STAGotIP;

unsigned long Wifi::configPortalStart = 0;
static bool restartPending = false;
static unsigned long restartAt = 0;
#ifdef WIFI_CONNECT_TIMEOUT
unsigned long Wifi::connectStart = 0;
#endif
bool Wifi::connect = false;
String Wifi::_ssid = "";
String Wifi::_pass = "";
DNSServer *Wifi::dnsServer = nullptr;

void Wifi::connectWifi()
{
    delay(50);
    if (globalConfig.wifi.ssid[0] != '\0')
    {
        setupWifi();
    }
    else
    {
        setupWifiManager(true);
    }
}

void Wifi::setupWifi()
{
    WiFi.persistent(false); // Solve possible wifi init errors (re-add at 6.2.1.16 #4044, #4083)
    WiFi.disconnect(true);  // Delete SDK wifi config
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoConnect(true);
    WiFi.setAutoReconnect(true);
    WiFi.hostname(UID);
    Debug::AddInfo(PSTR("WIFI  Connecting to %s"), globalConfig.wifi.ssid);
    STAGotIP = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &event) {
#ifdef WIFI_CONNECT_TIMEOUT
        connectStart = 0;
#endif
        Debug::AddInfo(PSTR("WIFI  Connected: %s, ip=%s"), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    });
    if (globalConfig.wifi.is_static)
    {
        IPAddress static_ip;
        IPAddress static_sn;
        IPAddress static_gw;
        static_ip.fromString(globalConfig.wifi.ip);
        static_sn.fromString(globalConfig.wifi.sn);
        static_gw.fromString(globalConfig.wifi.gw);
        Debug::AddInfo(PSTR("WIFI  Static ip=%s, gateway=%s, mask=%s"), globalConfig.wifi.ip, globalConfig.wifi.sn, globalConfig.wifi.gw);
        WiFi.config(static_ip, static_gw, static_sn);
    }

#ifdef WIFI_CONNECT_TIMEOUT
    connectStart = millis();
#endif
    WiFi.begin(globalConfig.wifi.ssid, globalConfig.wifi.pass);
}

void Wifi::setupWifiManager(bool resetSettings)
{
    if (resetSettings)
    {
        Config::resetConfig();

        WiFi.disconnect(true);
    }
    WiFi.hostname(UID);
    STAGotIP = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &event) {
        Debug::AddInfo(PSTR("WIFI  Connected: %s, ip=%s"), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    });

    configPortalStart = millis();
    if (WiFi.isConnected())
    {
        WiFi.mode(WIFI_AP_STA);
    }
    else
    {
        WiFi.persistent(false);
        WiFi.disconnect();
        WiFi.mode(WIFI_AP_STA);
        WiFi.persistent(true);
    }

    connect = false;
    WiFi.softAP(UID);
    delay(500);
    Debug::AddInfo(PSTR("WIFI  Setup hotspot ip=%s"), WiFi.softAPIP().toString().c_str());

    if (!dnsServer) dnsServer = new DNSServer();
    else dnsServer->stop();
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(53, "*", WiFi.softAPIP());
}

void Wifi::tryConnect(String ssid, String pass)
{
    _ssid = ssid;
    _pass = pass;
    connect = true;
}

void Wifi::loop()
{
    if (restartPending && (int32_t)(millis() - restartAt) >= 0)
    {
        restartPending = false;
        if (Config::prepareRestart()) ESP.restart();
        return;
    }
#ifdef WIFI_CONNECT_TIMEOUT
    if (connectStart > 0 && millis() > connectStart + (WIFI_CONNECT_TIMEOUT * 1000))
    {
        connectStart = 0;
        if (!WiFi.isConnected())
        {
            setupWifiManager(false);
            return;
        }
    }
#endif
    if (configPortalStart == 0)
    {
        return;
    }
    dnsServer->processNextRequest();

    if (connect)
    {
        connect = false;
        Debug::AddInfo(PSTR("WIFI  Connecting to selected network"));

        WiFi.begin(_ssid.c_str(), _pass.c_str());
    }

    if (_ssid.length() > 0 && WiFi.isConnected())
    {
        strcpy(globalConfig.wifi.ssid, _ssid.c_str());
        strcpy(globalConfig.wifi.pass, _pass.c_str());
        static unsigned long retryAt = 0;
        if (retryAt && (int32_t)(millis() - retryAt) < 0) return;
        if (!Config::saveConfig())
        {
            retryAt = millis() + 60000;
            Debug::AddError(PSTR("WIFI  Save failed; setup hotspot stays open"));
            return;
        }
        retryAt = 0;

        //	为了使WEB获取到IP 2秒后才关闭AP
        restartPending = true;
        restartAt = millis() + 3000;

        Debug::AddInfo(PSTR("WIFI  Network saved: %s, ip=%s; restarting"), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

        dnsServer->stop();
        configPortalStart = 0;
        _ssid = "";
        _pass = "";
        return;
    }

    // 检查是否超时
    if (millis() > configPortalStart + (WIFI_PORTAL_TIMEOUT * 1000))
    {
        dnsServer->stop();
        configPortalStart = 0;
        _ssid = "";
        _pass = "";
        Debug::AddInfo(PSTR("WIFI  Setup timed out"));
        if (WiFi.isConnected())
        {
            //	为了使WEB获取到IP 2秒后才关闭AP
            restartPending = true;
            restartAt = millis() + 3000;
        }
        else
        {
            Debug::AddInfo(PSTR("WIFI  Connection timed out; restarting"));
            restartPending = true;
            restartAt = millis() + 3000;
        }
    }
}

bool Wifi::isIp(const String &str)
{
    if (str.length() < 7 || str.length() > 15) return false;
    uint16_t value = 0;
    uint8_t dots = 0, digits = 0;
    for (size_t i = 0; i < str.length(); ++i)
    {
        char c = str[i];
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            if (++digits > 3 || value > 255) return false;
        }
        else if (c == '.' && digits && dots < 3)
        {
            ++dots;
            digits = 0;
            value = 0;
        }
        else return false;
    }
    return dots == 3 && digits != 0;
}
