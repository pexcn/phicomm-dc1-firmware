#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "GlobalConfig.pb.h"

#define F(text) text
#define PSTR(text) text
class String : public std::string {
public:
    using std::string::string;
    String(const std::string &value) : std::string(value) {}
    bool equals(const char *value) const { return *this == value; }
    int indexOf(char value) const { auto pos = find(value); return pos == npos ? -1 : int(pos); }
    void remove(size_t pos) { if (pos < size()) erase(pos); }
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
    const String &hostHeader() const { return host; }
    Client &client() { return connection; }
    void sendHeader(const char *, const String &, bool) {}
    void send(int code, const char *type, const char *payload) { send_P(code, type, payload); }
    void send_P(int code, const char *, const char *payload) { status = code; response = payload; }
};
static GlobalConfigMessage globalConfig;
static Server endpoint;
static bool authorized = true, saveOk = true;
static unsigned saves;
struct Http {
    static Server *server;
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
#include "network-handler.inc"

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

int main()
{
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
