#include <ESP8266WebServer.h>
#include <ctype.h>
#include "FirmwareUpload.h"
#include <WiFiUdp.h>
#include "Http.h"
#include "WebAuth.h"
#include "Module.h"
#include "Rtc.h"
#include "WebAssets.h"

ESP8266WebServer *Http::server;
bool Http::isBegin = false;

namespace {
static_assert(sizeof(globalConfig.http.pass) == WebAuth::PASSWORD_SIZE, "Password record size mismatch");
WebAuth webAuth;

const char *sessionToken(char token[WebAuth::TOKEN_SIZE])
{
    token[0] = 0;
    const String &cookies = Http::server->header(F("Cookie"));
    if (cookies.length() == 0) return token;
    const char *item = cookies.c_str();
    const char *limit = item + cookies.length();
    while (item < limit)
    {
        const char *separator = static_cast<const char *>(memchr(item, ';', limit - item));
        const char *end = separator ? separator : limit;
        while (item < end && isspace(static_cast<unsigned char>(*item))) ++item;
        while (end > item && isspace(static_cast<unsigned char>(end[-1]))) --end;
        const size_t prefixLength = sizeof("dc1_session=") - 1;
        if (size_t(end - item) >= prefixLength
            && memcmp_P(item, PSTR("dc1_session="), prefixLength) == 0)
        {
            item += prefixLength;
            if (size_t(end - item) == WebAuth::TOKEN_SIZE - 1)
            {
                memcpy(token, item, WebAuth::TOKEN_SIZE - 1);
                token[WebAuth::TOKEN_SIZE - 1] = 0;
            }
            return token;
        }
        if (!separator) break;
        item = separator + 1;
    }
    return token;
}
void clearCookie()
{
    Http::server->sendHeader(F("Set-Cookie"),
        F("dc1_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"));
}
bool clientRequest(bool respond = true)
{
    if (Http::server->header(F("X-DC1-Request")) == F("1")) return true;
    if (respond) Http::server->send_P(403, PSTR("application/json"),
        PSTR("{\"code\":0,\"msg\":\"请求来源无效，请使用设备管理页面。\"}"));
    return false;
}
bool issueSession()
{
    char token[WebAuth::TOKEN_SIZE];
    if (!webAuth.issue(token))
    {
        Http::server->send_P(503, PSTR("application/json"),
            PSTR("{\"code\":0,\"msg\":\"无法创建登录会话，请稍后重试。\"}"));
        return false;
    }
    Http::server->sendHeader(F("Set-Cookie"), String(F("dc1_session=")) + token
        + F("; Path=/; HttpOnly; SameSite=Strict"));
    return true;
}
void authStatus()
{
    Http::server->sendHeader(F("Cache-Control"), F("no-store"));
    if (!clientRequest()) return;
    char token[WebAuth::TOKEN_SIZE];
    bool authenticated = webAuth.authorize(sessionToken(token));
    snprintf_P(tmpData, sizeof(tmpData),
        PSTR("{\"code\":1,\"data\":{\"authenticated\":%d,\"setup_required\":%d}}"),
        authenticated, globalConfig.http.pass[0] == 0);
    Http::server->send(200, F("application/json"), tmpData);
}
bool passwordAttempt()
{
    String password = Http::server->arg(F("password"));
    if (password.length() == strlen(password.c_str())
        && WebAuth::verifyPassword(password.c_str(), globalConfig.http.pass))
    {
        return true;
    }
    Http::server->send_P(401, PSTR("application/json"),
        PSTR("{\"code\":0,\"msg\":\"密码不正确，请重试。\"}"));
    return false;
}
bool storePassword(const String &password)
{
    if (password.length() < 8 || password.length() > 64
        || password.length() != strlen(password.c_str()))
    {
        Http::server->send_P(400, PSTR("application/json"),
            PSTR("{\"code\":0,\"msg\":\"密码过短或过长，请使用 8–64 位字母、数字或符号。\"}"));
        return false;
    }
    char record[WebAuth::PASSWORD_SIZE];
    if (!WebAuth::makePassword(password.c_str(), record))
    {
        Http::server->send_P(503, PSTR("application/json"),
            PSTR("{\"code\":0,\"msg\":\"无法设置密码，请稍后重试。\"}"));
        return false;
    }
    char previous[sizeof(globalConfig.http.pass)];
    memcpy(previous, globalConfig.http.pass, sizeof(previous));
    memcpy(globalConfig.http.pass, record, sizeof(record));
    // Keep the old module payload intact, even before LittleFS is initialized.
    if (!Config::saveConfig(false))
    {
        memcpy(globalConfig.http.pass, previous, sizeof(previous));
        Http::server->send_P(500, PSTR("application/json"),
            PSTR("{\"code\":0,\"msg\":\"密码保存失败，未应用本次修改，请稍后重试。\"}"));
        return false;
    }
    return true;
}
void login()
{
    Http::server->sendHeader(F("Cache-Control"), F("no-store"));
    if (!clientRequest()) return;
    if (globalConfig.http.pass[0] == 0)
    {
        if (!storePassword(Http::server->arg(F("password")))) return;
    }
    else if (!passwordAttempt()) return;
    if (!issueSession()) return;
    Http::server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1}"));
}
void logout()
{
    if (!Http::checkAuth()) return;
    char token[WebAuth::TOKEN_SIZE];
    webAuth.revoke(sessionToken(token));
    clearCookie();
    Http::server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1}"));
}
void changePassword()
{
    if (!Http::checkAuth() || !passwordAttempt()) return;
    if (Http::server->arg(F("new_password")) == Http::server->arg(F("password")))
    {
        Http::server->send_P(200, PSTR("application/json"),
            PSTR("{\"code\":1,\"msg\":\"密码未改变，无需保存。\"}"));
        return;
    }
    if (!storePassword(Http::server->arg(F("new_password")))) return;
    webAuth.clear();
    if (!issueSession()) return;
    Http::server->send_P(200, PSTR("application/json"),
        PSTR("{\"code\":1,\"msg\":\"管理密码已修改，其他设备需要重新登录。\"}"));
}

bool argumentFits(const char *name, size_t capacity)
{
    if (Http::server->arg(name).length() < capacity) return true;
    Http::server->send_P(400, PSTR("application/json"),
        PSTR("{\"code\":0,\"msg\":\"输入内容超出设备支持的长度，请缩短后重试。\"}"));
    return false;
}

// Bound temporary allocations to one field rather than assembling the whole page.
void sendJsonField(const char *key, const String &value)
{
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i)
    {
        unsigned char ch = value[i];
        if (ch == '"' || ch == '\\') { escaped += '\\'; escaped += char(ch); }
        else if (ch < 0x20)
        {
            char code[7];
            snprintf(code, sizeof(code), "\\u%04x", ch);
            escaped += code;
        }
        else escaped += char(ch);
    }
    Http::server->sendContent(String('"') + key + F("\":\"") + escaped + F("\","));
}

void sendWebAsset(const uint8_t *asset, size_t length, const char *contentType)
{
    Http::server->sendHeader(F("Content-Encoding"), F("gzip"));
    Http::server->sendHeader(F("Cache-Control"), F("no-cache"));
    Http::server->send_P(200, contentType, reinterpret_cast<const char *>(asset), length);
}
}

void Http::handleRoot()
{
    if (captivePortal()) return;
    sendWebAsset(WebIndex, sizeof(WebIndex), PSTR("text/html; charset=utf-8"));
}

void Http::handleConfig()
{
    if (!checkAuth()) return;
    server->sendHeader(F("Cache-Control"), F("no-store"));
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1,\"data\":{\"features\":{"));
    snprintf_P(tmpData, sizeof(tmpData), PSTR("\"module\":%d"), module != nullptr);
    server->sendContent_P(tmpData);
#ifndef DISABLE_MQTT
    server->sendContent_P(PSTR(",\"mqtt\":1"));
#ifndef DISABLE_MQTT_DISCOVERY
    server->sendContent_P(PSTR(",\"discovery\":1"));
#endif
#endif
#ifdef WEB_LOG_SIZE
    server->sendContent_P(PSTR(",\"logs\":1"));
#endif
    server->sendContent_P(PSTR("},\"device\":{"));
    sendJsonField("uid", UID);
    sendJsonField("ssid", WiFi.SSID());
    sendJsonField("local_ip", server->client().localIP().toString());
    sendJsonField("wifi_mode", WiFi.getMode() == WIFI_STA ? F("STA") : WiFi.getMode() == WIFI_AP ? F("AP") : F("AP + STA"));
    sendJsonField("version", F(FIRMWARE_VERSION));
    sendJsonField("build_time", Rtc::GetBuildDateAndTime());
    sendJsonField("firmware_id", F(FIRMWARE_ID));
    sendJsonField("core_sdk", String(ARDUINO_ESP8266_RELEASE) + F(" / ") + ESP.getSdkVersion());
    sendJsonField("reset_reason", ESP.getResetReason());
    sendJsonField("mac", WiFi.macAddress());
    snprintf_P(tmpData, sizeof(tmpData),
        PSTR("\"flash_size\":%u,\"flash_config_size\":%u,\"sketch_size\":%u,\"ota_space\":%u,"
             "\"chip_id\":%u,\"flash_id\":%u,\"portal\":%d},\"fields\":{"),
        ESP.getFlashChipRealSize() / 1024, ESP.getFlashChipSize() / 1024,
        ESP.getSketchSize() / 1024, FirmwareUpload::capacity((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000) / 1024,
        ESP.getChipId(), ESP.getFlashChipId(), server->client().localIP() == WiFi.softAPIP());
    server->sendContent_P(tmpData);
    sendJsonField("uid", UID);
    sendJsonField("wifi_ssid", globalConfig.wifi.ssid);
    sendJsonField("static_ip", globalConfig.wifi.ip);
    sendJsonField("static_netmask", globalConfig.wifi.sn);
    sendJsonField("static_gateway", globalConfig.wifi.gw);
    sendJsonField("ntp", globalConfig.wifi.ntp);
#ifndef DISABLE_MQTT
    sendJsonField("mqtt_server", globalConfig.mqtt.server);
    sendJsonField("mqtt_username", globalConfig.mqtt.user);
    sendJsonField("mqtt_password", globalConfig.mqtt.pass);
    sendJsonField("mqtt_topic", globalConfig.mqtt.topic);
    sendJsonField("discovery_prefix", globalConfig.mqtt.discovery_prefix);
#endif
    snprintf_P(tmpData, sizeof(tmpData),
        PSTR("\"dhcp\":%d,\"mqtt_port\":%u,\"interval\":%u,\"retain\":%d,"
             "\"log_serial\":%d,\"log_serial1\":%d,\"log_web\":%d},\"module\":"),
        globalConfig.wifi.is_static ? 2 : 1, globalConfig.mqtt.port, globalConfig.mqtt.interval,
        globalConfig.mqtt.retain, !!(globalConfig.debug.type & 1), !!(globalConfig.debug.type & 8),
        !!(globalConfig.debug.type & 4));
    server->sendContent_P(tmpData);
    server->sendContent(module ? module->httpGetConfig() : String(F("{}")));
    server->sendContent_P(PSTR("}}"));
}

#ifndef DISABLE_MQTT
void Http::handleMqtt()
{
    if (!checkAuth())
    {
        return;
    }
    if (!argumentFits("mqtt_server", sizeof(globalConfig.mqtt.server))
        || !argumentFits("mqtt_username", sizeof(globalConfig.mqtt.user))
        || !argumentFits("mqtt_password", sizeof(globalConfig.mqtt.pass))
        || !argumentFits("mqtt_topic", sizeof(globalConfig.mqtt.topic))) return;
    String topic = server->arg(F("mqtt_topic"));
    if (topic.length() == 0)
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"MQTT主题不能为空\"}"));
        return;
    }
    if (topic.indexOf("%prefix%/") == 0)
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"MQTT主题必须包含【%prefix%/】\"}"));
        return;
    }
    strcpy(globalConfig.mqtt.server, server->arg(F("mqtt_server")).c_str());
    globalConfig.mqtt.port = server->arg(F("mqtt_port")).toInt();
    globalConfig.mqtt.retain = server->arg(F("retain")) == F("1");
    strcpy(globalConfig.mqtt.user, server->arg(F("mqtt_username")).c_str());
    strcpy(globalConfig.mqtt.pass, server->arg(F("mqtt_password")).c_str());
    strcpy(globalConfig.mqtt.topic, topic.c_str());
    globalConfig.mqtt.interval = server->arg(F("interval")).toInt();
    if (!saveConfigOrError()) return;

    if (Mqtt::mqttClient.connected())
    {
        Mqtt::mqttClient.disconnect();
    }

    if (Mqtt::mqttConnect())
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"设置MQTT服务器成功，已连接。\",\"data\":{\"mqttconnected\":\"已连接\"}}"));
    }
    else
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"设置MQTT服务器成功，未连接。\",\"data\":{\"mqttconnected\":\"未连接\"}}"));
    }
}

#ifndef DISABLE_MQTT_DISCOVERY
void Http::handleDiscovery()
{
    if (!checkAuth())
    {
        return;
    }
    if (!argumentFits("discovery_prefix", sizeof(globalConfig.mqtt.discovery_prefix))) return;
    strcpy(globalConfig.mqtt.discovery_prefix, server->arg(F("discovery_prefix")).c_str());
    globalConfig.mqtt.discovery = !globalConfig.mqtt.discovery;
    if (!saveConfigOrError()) return;

    if (module)
    {
        module->mqttDiscovery(globalConfig.mqtt.discovery);
    }
    if (globalConfig.mqtt.discovery)
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"已经打开MQTT自发现。\",\"data\":{\"discovery\":1}}"));
    }
    else
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"已经关闭MQTT自发现。\",\"data\":{\"discovery\":0}}"));
    }
}
#endif
#endif

void Http::handledhcp()
{
    if (!checkAuth())
    {
        return;
    }
    if (!argumentFits("static_ip", sizeof(globalConfig.wifi.ip))
        || !argumentFits("static_netmask", sizeof(globalConfig.wifi.sn))
        || !argumentFits("static_gateway", sizeof(globalConfig.wifi.gw))) return;
    bool isStatic = server->arg(F("dhcp")).equals(F("2"));
    String ip = server->arg(F("static_ip"));
    String netmask = server->arg(F("static_netmask"));
    String gateway = server->arg(F("static_gateway"));
    if (isStatic && !Wifi::isIp(ip))
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"IP地址错误\"}"));
        return;
    }
    if (isStatic && !Wifi::isIp(netmask))
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"掩码地址错误\"}"));
        return;
    }
    if (isStatic && !Wifi::isIp(gateway))
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"网关地址错误\"}"));
        return;
    }

    IPAddress static_ip;
    IPAddress static_sn;
    IPAddress static_gw;
    static_ip.fromString(ip);
    static_sn.fromString(netmask);
    static_gw.fromString(gateway);

    if (isStatic && !(static_ip.isV4() && static_sn.isV4() && (!static_gw.isSet() || static_gw.isV4())))
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"IP地址或者网关错误\"}"));
        return;
    }

    if (isStatic)
    {
        auto addressValue = [](const IPAddress &address) {
            return (uint32_t(address[0]) << 24) | (uint32_t(address[1]) << 16)
                | (uint32_t(address[2]) << 8) | address[3];
        };
        const uint32_t mask = addressValue(static_sn), hostMask = ~mask;
        if (!mask || (hostMask & (hostMask + 1)))
        {
            server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"子网掩码必须非零且连续。\"}"));
            return;
        }
        auto isHost = [&](const IPAddress &address) {
            const uint8_t first = address[0];
            const uint32_t host = addressValue(address) & hostMask;
            // /31 point-to-point and /32 host routes have no directed broadcast.
            return first != 0 && first != 127 && first < 224
                && (hostMask <= 1 || (host != 0 && host != hostMask));
        };
        if (!isHost(static_ip) || !isHost(static_gw))
        {
            server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"IP和网关必须为有效单播主机地址，不能为网络或广播地址。\"}"));
            return;
        }
        if ((static_ip.v4() & static_sn.v4()) != (static_gw.v4() & static_sn.v4()))
        {
            server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"网段错误\"}"));
            return;
        }
    }

    const bool restartRequired = globalConfig.wifi.is_static != isStatic
        || (isStatic && (strcmp(globalConfig.wifi.ip, ip.c_str()) != 0
            || strcmp(globalConfig.wifi.sn, netmask.c_str()) != 0
            || strcmp(globalConfig.wifi.gw, gateway.c_str()) != 0));
    globalConfig.wifi.is_static = isStatic;
    strcpy(globalConfig.wifi.ip, ip.c_str());
    strcpy(globalConfig.wifi.sn, netmask.c_str());
    strcpy(globalConfig.wifi.gw, gateway.c_str());
    if (!saveConfigOrError()) return;

    if (restartRequired)
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"网络设置已保存，请在系统维护中重启设备后生效。\"}"));
    }
    else
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"网络设置已保存。\"}"));
    }
}

void Http::handleScanWifi()
{
    if (!checkAuth())
    {
        return;
    }
    int n = WiFi.scanNetworks();
    if (n < 0)
    {
        server->send_P(503, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"Wi-Fi 搜索失败，请稍后重试。\"}"));
        return;
    }
    if (n == 0)
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"\",\"data\":{\"list\":[]}}"));
        return;
    }

    //sort networks
    int indices[n];
    for (int i = 0; i < n; i++)
    {
        indices[i] = i;
    }

    // RSSI排序
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i]))
            {
                std::swap(indices[i], indices[j]);
            }
        }
    }

    // 删除重复项（必须对RSSI进行排序）
    String cssid;
    for (int i = 0; i < n; i++)
    {
        if (indices[i] == -1)
            continue;
        cssid = WiFi.SSID(indices[i]);
        for (int j = i + 1; j < n; j++)
        {
            if (cssid == WiFi.SSID(indices[j]))
            {
                indices[j] = -1; // set dup aps to index -1
            }
        }
    }

    int _minimumQuality = -1;
    int quality;
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"\",\"data\":{\"list\":["));
    bool firstNetwork = true;
    for (int i = 0; i < n; i++)
    {
        if (indices[i] == -1)
            continue; // skip dups
        int RSSI = WiFi.RSSI(indices[i]);
        if (RSSI <= -100)
        {
            quality = 0;
        }
        else if (RSSI >= -50)
        {
            quality = 100;
        }
        else
        {
            quality = 2 * (RSSI + 100);
        }
        if (_minimumQuality == -1 || _minimumQuality < quality)
        {
            if (!firstNetwork) server->sendContent_P(PSTR(","));
            firstNetwork = false;
            server->sendContent_P(PSTR("{"));
            sendJsonField("name", WiFi.SSID(indices[i]));
            snprintf_P(tmpData, sizeof(tmpData), PSTR("\"rssi\":%d,\"quality\":%d,\"type\":%d}"), RSSI, quality, WiFi.encryptionType(indices[i]));
            server->sendContent_P(tmpData);
        }
    }

    server->sendContent_P(PSTR("]}}"));
}

void Http::handleWifi()
{
    if (!checkAuth())
    {
        return;
    }
    if (!argumentFits("wifi_ssid", sizeof(globalConfig.wifi.ssid))
        || !argumentFits("wifi_password", sizeof(globalConfig.wifi.pass))) return;
    String wifi = server->arg(F("wifi_ssid"));
    if (wifi == "")
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":0,\"msg\":\"WiFi名称不能为空。\"}"));
        return;
    }
    String password = server->arg(F("wifi_password"));

    if (WiFi.getMode() == WIFI_STA)
    {
        strcpy(globalConfig.wifi.ssid, wifi.c_str());
        strcpy(globalConfig.wifi.pass, password.c_str());
        if (!saveConfigOrError()) return;
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"设置WiFi信息成功，重启模块（手动）使用新的Wifi信息连接。\"}"));
    }
    else
    {
        Wifi::tryConnect(wifi, password);
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"尝试将ESP连接到网络。 如果失败，请重新连接到AP再试一次。\"}"));
    }
}

void Http::handleOperate()
{
    if (!checkAuth())
    {
        return;
    }
    String d = server->arg(F("d")); // 1：重启模块 2：重置模块
    if (d == F("1"))
    {
        if (!prepareRestartOrError()) return;
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"设备正在重启 . . .\"}"));
    }
    else if (d == F("2"))
    {
        if (module && !module->resetRuntime())
        {
            server->send_P(500, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"运行数据重置未能保存；请先检查或初始化存储，设备未重启。\"}"));
            return;
        }
        Config::resetConfig();
        if (!saveConfigOrError()) return;
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"正在重置模块 . . . 设备将会重启。\"}"));
    }
    else
    {
        server->send_P(400, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"无效的设备操作。\"}"));
        return;
    }
    delay(200);

    Led::blinkLED(400, 4);
    ESP.restart();
}

void Http::handleNotFound()
{
    if (captivePortal())
    {
        return;
    }
    server->sendHeader(PSTR("Cache-Control"), PSTR("no-cache, no-store, must-revalidate"));
    server->sendHeader(PSTR("Pragma"), PSTR("no-cache"));
    server->sendHeader(PSTR("Expires"), PSTR("-1"));
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);

    snprintf_P(tmpData, sizeof(tmpData), PSTR("File Not Found\n\nURI: %s\nMethod: %s\nArguments: %d\n"),
               server->uri().c_str(), server->method() == HTTP_GET ? PSTR("GET") : PSTR("POST"), server->args());
    server->send_P(404, PSTR("text/plain"), tmpData);
    for (uint8_t i = 0; i < server->args(); i++)
    {
        snprintf_P(tmpData, sizeof(tmpData), PSTR(" %s: %s\n"), server->argName(i).c_str(), server->arg(i).c_str());
        server->sendContent_P(tmpData);
    }
}

void Http::handleGetStatus()
{
    if (!checkAuth())
    {
        return;
    }

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"\",\"data\":{"));

    snprintf_P(tmpData, sizeof(tmpData), PSTR("\"uptime\":\"%s\",\"free_mem\":%d,\"rssi\":%d"), Rtc::msToHumanString(millis()).c_str(), ESP.getFreeHeap() / 1024, WiFi.RSSI());
    server->sendContent_P(tmpData);

#ifndef DISABLE_MQTT
    if (Mqtt::mqttClient.connected())
    {
        server->sendContent_P(PSTR(",\"mqttconnected\":\"已连接\""));
    }
    else
    {
        server->sendContent_P(PSTR(",\"mqttconnected\":\"未连接\""));
    }

#ifndef DISABLE_MQTT_DISCOVERY
    snprintf_P(tmpData, sizeof(tmpData), PSTR(",\"discovery\":%d"), globalConfig.mqtt.discovery ? 1 : 0);
    server->sendContent_P(tmpData);
#endif
#endif

    if (Wifi::configPortalStart == 0 && WiFi.isConnected())
    {
        snprintf_P(tmpData, sizeof(tmpData), PSTR(",\"ip\":\"%s\""), WiFi.localIP().toString().c_str());
        server->sendContent_P(tmpData);
    }

    if (module)
    {
        String tmp = module->httpGetStatus(server);
        if (tmp.length() > 0)
        {
            server->sendContent_P(PSTR(","));
            server->sendContent(tmp);
        }
    }

#ifdef WEB_LOG_SIZE
    if (server->hasArg(F("i")))
    {
        bool cflg = false; // Separate records within this response; the UI joins batches.
        uint8_t counter = server->arg(F("i")).toInt();
        snprintf_P(tmpData, sizeof(tmpData), PSTR(",\"logindex\":%d,\"log\":\""), Debug::webLogIndex);
        server->sendContent_P(tmpData);
        if (counter != Debug::webLogIndex)
        {
            if (!counter)
            {
                counter = Debug::webLogIndex;
            }
            do
            {
                char *tmp;
                uint16_t len;
                Debug::GetLog(counter, &tmp, &len);
                if (len)
                {
                    if (cflg)
                    {
                        server->sendContent_P(PSTR("\\n"));
                    }

                    size_t j = 0;
                    for (uint16_t i = 0; i + 1 < len; i++)
                    {
                        // Stream long escaped lines without another buffer or String.
                        if (j >= sizeof(tmpData) - 2)
                        {
                            server->sendContent(tmpData, j);
                            j = 0;
                        }
                        char each = tmp[i];
                        bool escape = true;
                        switch (each)
                        {
                        case '\\': case '"': break;
                        case '\b': each = 'b'; break;
                        case '\f': each = 'f'; break;
                        case '\n': each = 'n'; break;
                        case '\r': each = 'r'; break;
                        case '\t': each = 't'; break;
                        default: escape = false; break;
                        }
                        if (escape) tmpData[j++] = '\\';
                        tmpData[j++] = each;
                    }
                    tmpData[j] = '\0';
                    if (j) server->sendContent(tmpData, j);
                    cflg = true;
                }
                counter++;
                if (!counter)
                {
                    counter++;
                } // Skip log index 0 as it is not allowed
            } while (counter != Debug::webLogIndex);
        }
        server->sendContent_P(PSTR("\""));
    }
#endif
    server->sendContent_P(PSTR("}}"));
}

void Http::handleUpdate()
{
    static bool uploadAllowed = false;
    static bool uploadComplete = false;
    static bool uploadSaveFailed = false;
    static bool uploadImageFailed = false;
    static FirmwareUpload firmware;
    // handler for the /update form POST (once file upload finishes)
    server->on(
        F("/update"), HTTP_POST, [&]() {
        if (!checkAuth())
        {
            return;
        } 
        bool complete = uploadAllowed && uploadComplete && !uploadImageFailed;
        bool saveFailed = uploadSaveFailed;
        bool imageFailed = uploadImageFailed;
        if (complete && !firmware.finish()) imageFailed = true;
        if (!complete) firmware.cancel();
        uploadAllowed = uploadComplete = uploadSaveFailed = uploadImageFailed = false;
        if (saveFailed)
        {
            server->send_P(500, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"运行数据保存失败，固件升级未开始；请检查或恢复记录。\"}"));
            return;
        }
        if (imageFailed)
        {
            server->send_P(400, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"固件不匹配、缺少校验信息或文件损坏，已拒绝升级。请使用本项目新版 Release 的 .bin 文件。\"}"));
            return;
        }
        if (!complete && !Update.hasError())
        {
            server->send_P(400, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"固件上传未完成，设备未重启。\"}"));
            return;
        }
        if (Update.hasError())
        {
            uint8_t _error = Update.getError();
            if(_error == UPDATE_ERROR_WRITE){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Flash Write Failed"), _error);
            } else if(_error == UPDATE_ERROR_ERASE){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Flash Erase Failed"), _error);
            } else if(_error == UPDATE_ERROR_READ){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Flash Read Failed"), _error);
            } else if(_error == UPDATE_ERROR_SPACE){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Not Enough Space"), _error);
            } else if(_error == UPDATE_ERROR_SIZE){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Bad Size Given"), _error);
            } else if(_error == UPDATE_ERROR_STREAM){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Stream Read Timeout"), _error);
            } else if(_error == UPDATE_ERROR_SIGN){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Signature verification failed"), _error);
            } else if(_error == UPDATE_ERROR_FLASH_CONFIG){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Flash config wrong real: %d IDE: %d"), _error,  ESP.getFlashChipRealSize(), ESP.getFlashChipSize());
            } else if(_error == UPDATE_ERROR_NEW_FLASH_CONFIG){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: new Flash config wrong real: %d"), _error, ESP.getFlashChipRealSize());
            } else if(_error == UPDATE_ERROR_MAGIC_BYTE){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Magic byte is wrong, not 0xE9"), _error);
            } else if (_error == UPDATE_ERROR_BOOTSTRAP){
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: Invalid bootstrapping state, reset ESP8266 before updating"), _error);
            } else {
                snprintf_P(tmpData, sizeof(tmpData), PSTR("OTA   Error %u: UNKNOWN"), _error);
            }
            Debug::AddLog(LOG_LEVEL_ERROR);
            char out[150] = {0};
            snprintf_P(out, sizeof(out), PSTR("{\"code\":0,\"msg\":\"%s\"}"), tmpData);
            server->send_P(200, PSTR("text/html"), out);
        }
        else
        {
            server->client().setNoDelay(true);
            server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"升级成功，设备将自动重启，稍后刷新页面即可。\"}"));
            delay(100);
            server->client().stop();
            ESP.restart();
        } }, [&]() {
                HTTPUpload &upload = server->upload();
                if (upload.status == UPLOAD_FILE_START)
                {
                    uploadAllowed = uploadComplete = uploadSaveFailed = uploadImageFailed = false;
                    firmware.cancel();
                    if (!checkAuth(false))
                    {
                        Debug::AddInfo(PSTR("OTA   Rejected: login required"));
                        return;
                    }
                    if (!Config::prepareRestart())
                    {
                        uploadSaveFailed = true;
                        return;
                    }
                    WiFiUDP::stopAll();
                    Debug::AddInfo(PSTR("OTA   Upload %s"), upload.filename.c_str());
                    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
                    uploadAllowed = firmware.begin(maxSketchSpace);
                }
                else if (upload.status == UPLOAD_FILE_WRITE && uploadAllowed && !uploadImageFailed && !Update.hasError())
                {
                    if (!firmware.write(upload.buf, upload.currentSize))
                    {
                        uploadImageFailed = true;
                        firmware.cancel();
                    }
                }
                else if (upload.status == UPLOAD_FILE_END && uploadAllowed && !Update.hasError())
                {
                    uploadComplete = !uploadImageFailed;
                    // Finalize only after the authenticated POST handler accepts the upload.
                }
                else if (upload.status == UPLOAD_FILE_ABORTED)
                {
                    firmware.cancel();
                    uploadAllowed = uploadComplete = false;
                    Debug::AddInfo(PSTR("OTA   Upload aborted"));
                }
                delay(0); });
}

void Http::begin()
{
    if (isBegin)
    {
        return;
    }
    isBegin = true;
    server = new ESP8266WebServer();
    server->collectHeaders("Cookie", "X-DC1-Request");
    server->on(F("/auth"), HTTP_POST, authStatus);
    server->on(F("/login"), HTTP_POST, login);
    server->on(F("/logout"), HTTP_POST, logout);
    server->on(F("/password"), HTTP_POST, changePassword);

    server->on(F("/"), HTTP_GET, handleRoot);
    server->on(F("/style.css"), HTTP_GET, []() {
        sendWebAsset(WebStyle, sizeof(WebStyle), PSTR("text/css; charset=utf-8"));
    });
    server->on(F("/app.js"), HTTP_GET, []() {
        sendWebAsset(WebScript, sizeof(WebScript), PSTR("application/javascript; charset=utf-8"));
    });
    server->on(F("/get_config"), HTTP_POST, handleConfig);
#ifndef DISABLE_MQTT
    server->on(F("/mqtt"), HTTP_POST, handleMqtt);
#ifndef DISABLE_MQTT_DISCOVERY
    server->on(F("/discovery"), HTTP_POST, handleDiscovery);
#endif
#endif
    server->on(F("/dhcp"), HTTP_POST, handledhcp);
    server->on(F("/scan_wifi"), HTTP_POST, handleScanWifi);
    server->on(F("/wifi"), HTTP_POST, handleWifi);
    server->on(F("/operate"), HTTP_POST, handleOperate);
    server->on(F("/module_setting"), HTTP_POST, handleModuleSetting);
    server->on(F("/get_status"), HTTP_POST, handleGetStatus);
    server->onNotFound(handleNotFound);
    handleUpdate();

    if (module)
    {
        module->httpAdd(server);
    }
    server->begin(globalConfig.http.port);
    Debug::AddInfo(PSTR("HTTP  Listening on port %u"), globalConfig.http.port);
}

void Http::stop()
{
    if (!isBegin)
    {
        return;
    }
    server->stop();
    Debug::AddInfo(PSTR("HTTP  Server stopped"));
}

void Http::loop()
{
    if (isBegin)
    {
        server->handleClient();
    }
}

bool Http::captivePortal()
{
    String host = server->hostHeader();
    const int port = host.indexOf(':');
    if (port >= 0) host.remove(port);
    if (!Wifi::isIp(host))
    {
        server->sendHeader(F("Location"), String(F("http://")) + server->client().localIP().toString(), true);
        server->send(302, F("text/plain"), ""); // Empty content inhibits Content-length header so we have to close the socket ourselves.
        server->client().stop();                // Stop is needed because we sent no content length
        return true;
    }
    return false;
}

void Http::handleModuleSetting()
{
    if (!checkAuth())
    {
        return;
    }

    if (!argumentFits("uid", sizeof(UID))
        || !argumentFits("ntp", sizeof(globalConfig.wifi.ntp))) return;
    int t = 0;
    if (server->arg(F("log_serial")).equals(F("1")))
    {
        t = t | 1;
    }
    if (server->arg(F("log_serial1")).equals(F("1")))
    {
        t = t | 8;
    }
#ifdef WEB_LOG_SIZE
    if (server->arg(F("log_web")).equals(F("1")))
    {
        t = t | 4;
    }
#endif


    globalConfig.debug.type = t;
    if ((8 & globalConfig.debug.type) == 8)
    {
        Serial1.begin(115200);
    }

    String ntp = server->arg(F("ntp"));
    if (strcmp(globalConfig.wifi.ntp, ntp.c_str()) != 0)
    {
        strcpy(globalConfig.wifi.ntp, ntp.c_str());
        Rtc::init();
    }

    String uid = server->arg(F("uid"));
    strcpy(globalConfig.uid, uid.c_str());
    if (!saveConfigOrError()) return;
    if (uid.length() == 0 || strcmp(globalConfig.uid, UID) != 0)
    {
        if (!prepareRestartOrError()) return;
#ifndef DISABLE_MQTT
#ifndef DISABLE_MQTT_DISCOVERY
        if (globalConfig.mqtt.discovery && module)
        {
            module->mqttDiscovery(false);
        }
#endif
#endif
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"修改了重要配置 . . . 正在重启中。\"}"));
        Led::blinkLED(400, 4);
        ESP.restart();
    }
    else
    {
        server->send_P(200, PSTR("text/html"), PSTR("{\"code\":1,\"msg\":\"已经修改成功\"}"));
    }
}

bool Http::resetPassword()
{
    if (!Config::resetWebPassword())
    {
        Debug::AddError(PSTR("AUTH  Reset failed; previous password retained in RAM"));
        return false;
    }
    webAuth.clear();
    Debug::AddInfo(PSTR("AUTH  Password reset; reopen login to set a new password"));
    return true;
}

bool Http::checkAuth(bool respond)
{
    if (respond) server->sendHeader(F("Cache-Control"), F("no-store"));
    if (server->method() != HTTP_GET && !clientRequest(respond)) return false;
    char token[WebAuth::TOKEN_SIZE];
    if (webAuth.authorize(sessionToken(token))) return true;
    if (respond)
    {
        clearCookie();
        server->send_P(401, PSTR("application/json"),
            PSTR("{\"code\":0,\"msg\":\"请先登录设备。\"}"));
    }
    return false;
}

bool Http::saveConfigOrError()
{
    if (Config::saveConfig()) return true;
    server->send_P(500, PSTR("application/json"),
        PSTR("{\"code\":0,\"msg\":\"保存失败，设置仅在内存中，将稍后重试；运行数据存储未就绪时，请先初始化。\"}"));
    return false;
}

bool Http::prepareRestartOrError()
{
    if (Config::prepareRestart()) return true;
    server->send_P(500, PSTR("application/json"),
        PSTR("{\"code\":0,\"msg\":\"保存失败，已取消重启或升级；请检查存储状态并恢复损坏记录。\"}"));
    return false;
}
