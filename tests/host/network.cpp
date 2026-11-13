#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include "GlobalConfig.pb.h"
#include "DC1Config.pb.h"

#define F(text) text
#define PSTR(text) text
#define CONTENT_LENGTH_UNKNOWN -1
using __FlashStringHelper = char;
class String : public std::string {
public:
    using std::string::string;
    String(const std::string &value) : std::string(value) {}
    bool equals(const char *value) const { return *this == value; }
    int indexOf(char value) const { auto pos = find(value); return pos == npos ? -1 : int(pos); }
    void remove(size_t pos) { if (pos < size()) erase(pos); }
    long toInt() const { return strtol(c_str(), nullptr, 10); }
};
class IPAddress {
    std::array<uint8_t, 4> bytes = {};
public:
    uint8_t &operator[](unsigned index) { return bytes.at(index); }
    uint8_t operator[](unsigned index) const { return bytes.at(index); }
    void setV4() {}
    bool isV4() const { return true; }
    uint32_t v4() const { uint32_t value; memcpy(&value, bytes.data(), 4); return value; }
    bool isSet() const { return v4() != 0 && v4() != UINT32_MAX; }
    bool fromString4(const char *address);
    bool fromString(const String &address) { return fromString4(address.c_str()); }
    String toString() const { return "192.168.1.100"; }
};
struct Client {
    bool stopped = false;
    IPAddress localIP() const { return {}; }
    void stop() { stopped = true; }
};
struct Server {
    std::map<std::string, String> args;
    int status = 0;
    std::string response;
    String host;
    Client connection;
    String arg(const char *name) { return args[name]; }
    bool hasArg(const char *name) const { return args.count(name) != 0; }
    const String &hostHeader() const { return host; }
    Client &client() { return connection; }
    void sendHeader(const char *, const String &, bool) {}
    void send(int code, const char *type, const char *payload) { send_P(code, type, payload); }
    void send_P(int code, const char *, const char *payload) { status = code; response = payload; }
    void setContentLength(int) {}
    void sendContent(const String &text) { response += text; }
    void sendContent_P(const char *text) { response += text; }
};
static GlobalConfigMessage globalConfig;
static Server endpoint;
static bool authorized = true, saveOk = true;
static unsigned saves;
struct Mqtt {
    struct Connection {
        bool connected() const { return true; }
        void disconnect() {}
    };
    static Connection mqttClient;
    static bool mqttConnect() { return true; }
};
Mqtt::Connection Mqtt::mqttClient;
struct Http {
    static Server *server;
    static void handleMqtt();
    static void handledhcp();
    static bool captivePortal();
    static bool checkAuth()
    {
        if (!authorized) server->send_P(401, "application/json", "{\"code\":0}");
        return authorized;
    }
    static bool saveConfigOrError()
    {
        ++saves;
        if (!saveOk) server->send_P(500, "application/json", "{\"code\":0}");
        return saveOk;
    }
};
Server *Http::server = &endpoint;
struct Wifi { static bool isIp(const String &str); };
using ESP8266WebServer = Server;
struct DC1 {
    DC1ConfigMessage config = {};
    bool relaySavePending = false, runtimeMigrationPending = false;
    unsigned ledUpdates = 0, relaySchedules = 0, energyClears = 0;
    void logoLed() { ++ledUpdates; }
    void scheduleRelaySave() { ++relaySchedules; relaySavePending = true; }
    bool energyClear() { ++energyClears; return true; }
    String httpGetStatus(Server *) { return ""; }
    void httpSetting(ESP8266WebServer *server);
};
#include "network-handler.inc"
#include "outlet-settings.inc"

static void setup(bool isStatic = true)
{
    globalConfig = {};
    globalConfig.wifi.is_static = isStatic;
    strcpy(globalConfig.wifi.ip, "192.168.1.100");
    strcpy(globalConfig.wifi.sn, "255.255.255.0");
    strcpy(globalConfig.wifi.gw, "192.168.1.1");
    strcpy(globalConfig.http.pass, "password-record");
    strcpy(globalConfig.mqtt.server, "mqtt.lan");
    endpoint = {};
    endpoint.args = {{"dhcp", isStatic ? "2" : "1"}, {"static_ip", globalConfig.wifi.ip},
        {"static_netmask", globalConfig.wifi.sn}, {"static_gateway", globalConfig.wifi.gw}};
    saves = 0;
    authorized = saveOk = true;
}

static void submit()
{
    Http::handledhcp();
    assert(endpoint.status == 200 && saves == 1);
    assert(endpoint.response.find("\"code\":1") != std::string::npos);
    assert(globalConfig.wifi.is_static == (endpoint.args["dhcp"] == "2"));
    assert(strcmp(globalConfig.wifi.ip, endpoint.args["static_ip"].c_str()) == 0);
    assert(strcmp(globalConfig.wifi.sn, endpoint.args["static_netmask"].c_str()) == 0);
    assert(strcmp(globalConfig.wifi.gw, endpoint.args["static_gateway"].c_str()) == 0);
    assert(strcmp(globalConfig.http.pass, "password-record") == 0);
    assert(strcmp(globalConfig.mqtt.server, "mqtt.lan") == 0);
}

static void testOutletSettings()
{
    const char *fields[] = {"power_on_state", "power_mode", "logo_led", "wifi_led", "sub_kinkage"};
    const unsigned maximum[] = {3, 1, 3, 2, 2};
    auto setupOutlet = [] {
        setup();
        endpoint.args = {{"power_on_state", "3"}, {"power_mode", "1"}, {"logo_led", "3"},
            {"wifi_led", "2"}, {"sub_kinkage", "2"}, {"report_interval", "120"},
            {"energy_power_delta", "20"}, {"energy_max_power", "2400"}};
    };
    auto reject = [&](const char *field, const String &value) {
        setupOutlet();
        endpoint.args[field] = value;
        DC1 dc;
        const auto before = dc.config;
        dc.httpSetting(&endpoint);
        assert(endpoint.status == 400 && saves == 0);
        assert(memcmp(&dc.config, &before, sizeof(before)) == 0);
        assert(dc.ledUpdates == 0 && dc.relaySchedules == 0 && !dc.relaySavePending);
    };
    for (unsigned field = 0; field < 5; ++field)
    {
        for (unsigned value : {0U, maximum[field]})
        {
            setupOutlet();
            endpoint.args[fields[field]] = std::to_string(value);
            DC1 dc;
            dc.httpSetting(&endpoint);
            const uint8_t actual[] = {dc.config.power_on_state, dc.config.power_mode,
                dc.config.logo_led, dc.config.wifi_led, dc.config.sub_kinkage};
            for (unsigned i = 0; i < 5; ++i) assert(actual[i] == (i == field ? value : maximum[i]));
            assert(saves == 1 && endpoint.status == 200 && dc.ledUpdates == 1);
            assert(dc.config.report_interval == 120 && dc.config.energy_power_delta == 20
                && dc.config.energy_max_power == 2400);
        }
        reject(fields[field], std::to_string(maximum[field] + 1));
    }
    // The shared parser's malformed inputs need only one representative field.
    for (const char *value : {"", "-1", "257", "1x", "abc", "01"}) reject("sub_kinkage", value);
    setupOutlet();
    endpoint.args.erase("sub_kinkage");
    DC1 dc;
    dc.httpSetting(&endpoint);
    assert(endpoint.status == 400 && saves == 0 && dc.ledUpdates == 0);
    setupOutlet(); authorized = false;
    dc.httpSetting(&endpoint);
    assert(endpoint.status == 401 && saves == 0);
    setupOutlet(); endpoint.args = {{"c", "1"}};
    dc.httpSetting(&endpoint);
    assert(endpoint.status == 200 && dc.energyClears == 1 && saves == 0 && dc.ledUpdates == 0);
    puts("PASS: actual outlet handler validates all five enums before configuration/hardware/save changes; valid settings and energy clearing remain supported");
}

int main()
{
    testOutletSettings();
    for (const char *topic : {"%module%/%hostname%/%prefix%/", "%prefix%/%hostname%/",
            "dc1/%prefix%", "devices/dc1"})
    {
        setup(); endpoint.args["mqtt_topic"] = topic;
        Http::handleMqtt();
        assert(saves == 1 && strcmp(globalConfig.mqtt.topic, topic) == 0);
        assert(endpoint.response.find("\"code\":1") != std::string::npos);
    }
    for (const auto &topic : {std::string(), std::string(sizeof(globalConfig.mqtt.topic), 'x')})
    {
        setup(); endpoint.args["mqtt_topic"] = topic;
        const auto before = globalConfig;
        Http::handleMqtt();
        assert(saves == 0 && memcmp(&globalConfig, &before, sizeof(before)) == 0);
    }
    for (const char *valid : {"0.0.0.0", "255.255.255.255", "192.168.100.200", "001.002.003.004"})
        assert(Wifi::isIp(valid));
    for (const char *invalid : {"1.2.3", "1..2.3", "1.2.3.", "1.2.3.4x", "1.2.3.4:80",
            "1.2.3.4 ", "+1.2.3.4", "-1.2.3.4", "0000.1.2.3", "256.1.2.3"})
        assert(!Wifi::isIp(invalid));
    assert(!Wifi::isIp(String("1.2.3.4\0x", 9)));
    setup(); submit();
    setup(false); endpoint.args["static_ip"] = "not-an-ip"; submit();
    setup();
    endpoint.args["static_ip"] = "192.168.100.200";
    endpoint.args["static_netmask"] = "255.255.255.128";
    endpoint.args["static_gateway"] = "192.168.100.129";
    submit();

    for (const char *mask : {"255.0.255.0", "255.255.0.255", "0.0.0.0", "255.255.255.1"})
    {
        setup(); endpoint.args["static_netmask"] = mask;
        auto before = globalConfig;
        Http::handledhcp();
        assert(saves == 0 && memcmp(&globalConfig, &before, sizeof(before)) == 0);
    }
    for (const char *field : {"static_ip", "static_gateway"})
    for (const char *address : {"192.168.1.0", "192.168.1.255", "255.255.255.255",
            "0.0.0.0", "0.1.2.3", "127.0.0.1", "224.0.0.1", "240.0.0.1"})
    {
        setup(); endpoint.args[field] = address;
        Http::handledhcp(); assert(saves == 0);
    }
    // Network/broadcast exclusions follow the mask, not a blanket .0/.255 ban.
    setup(); endpoint.args["static_ip"] = "192.168.1.0";
    endpoint.args["static_netmask"] = "255.255.254.0"; submit();
    setup(); endpoint.args["static_ip"] = "192.168.0.255";
    endpoint.args["static_netmask"] = "255.255.254.0"; submit();
    setup(); endpoint.args["static_ip"] = "192.168.1.0";
    endpoint.args["static_netmask"] = "255.255.255.254"; submit();
    setup(); endpoint.args["static_gateway"] = "192.168.1.100";
    endpoint.args["static_netmask"] = "255.255.255.255"; submit();
    for (unsigned prefix = 1; prefix <= 32; ++prefix)
    {
        setup();
        const uint32_t mask = UINT32_MAX << (32 - prefix);
        char text[16];
        snprintf(text, sizeof(text), "%u.%u.%u.%u", unsigned(mask >> 24),
            unsigned(mask >> 16 & 255), unsigned(mask >> 8 & 255), unsigned(mask & 255));
        endpoint.args["static_netmask"] = text;
        endpoint.args["static_ip"] = prefix == 31 ? "10.1.2.0" : "10.1.2.1";
        endpoint.args["static_gateway"] = prefix >= 31 ? "10.1.2.1" : "10.1.2.2";
        submit();
    }
    for (const char *host : {"192.168.1.100", "192.168.1.100:80", "192.168.1.100:8080"})
    {
        setup(); endpoint.host = host;
        assert(!Http::captivePortal() && !endpoint.connection.stopped);
    }
    setup(); endpoint.host = "example.com:80";
    assert(Http::captivePortal() && endpoint.status == 302 && endpoint.connection.stopped);

    for (const char *field : {"static_ip", "static_netmask", "static_gateway"})
    for (const char *invalid : {"256.168.1.100", "192.168.100.2000"})
    {
        setup(); endpoint.args[field] = invalid;
        const auto before = globalConfig;
        Http::handledhcp();
        assert(saves == 0 && endpoint.response.find("\"code\":0") != std::string::npos);
        assert(memcmp(&globalConfig, &before, sizeof(before)) == 0);
    }
    setup(); endpoint.args["static_gateway"] = "192.168.2.1";
    Http::handledhcp(); assert(saves == 0);
    setup(); authorized = false;
    Http::handledhcp(); assert(endpoint.status == 401 && saves == 0);
    setup(); saveOk = false; endpoint.args["static_ip"] = "192.168.1.200";
    Http::handledhcp();
    assert(endpoint.status == 500 && saves == 1);
    assert(endpoint.response.find("\"code\":1") == std::string::npos);
    puts("PASS: actual network handler validates IPv4/subnet/host boundaries and leaves configuration unchanged on rejected requests");
}
