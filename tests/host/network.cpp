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
};
class IPAddress {
    std::array<uint8_t, 4> bytes = {};
public:
    uint8_t &operator[](unsigned index) { return bytes.at(index); }
    void setV4() {}
    bool isV4() const { return true; }
    uint32_t v4() const { uint32_t value; memcpy(&value, bytes.data(), 4); return value; }
    bool isSet() const { return v4() != 0 && v4() != UINT32_MAX; }
    bool fromString4(const char *address);
    bool fromString(const String &address) { return fromString4(address.c_str()); }
};
struct Server {
    std::map<std::string, String> args;
    int status = 0;
    std::string response;
    String arg(const char *name) { return args[name]; }
    void send_P(int code, const char *, const char *payload) { status = code; response = payload; }
};
static GlobalConfigMessage globalConfig;
static Server endpoint;
static bool authorized = true, saveOk = true;
static unsigned saves;
struct Http {
    static Server *server;
    static void handledhcp();
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
struct Wifi { static bool isIp(String str); };
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

static void submit(bool restartRequired)
{
    Http::handledhcp();
    assert(endpoint.status == 200 && saves == 1);
    assert(endpoint.response.find("\"code\":1") != std::string::npos);
    assert((endpoint.response.find("重启") != std::string::npos) == restartRequired);
    assert(globalConfig.wifi.is_static == (endpoint.args["dhcp"] == "2"));
    assert(strcmp(globalConfig.wifi.ip, endpoint.args["static_ip"].c_str()) == 0);
    assert(strcmp(globalConfig.wifi.sn, endpoint.args["static_netmask"].c_str()) == 0);
    assert(strcmp(globalConfig.wifi.gw, endpoint.args["static_gateway"].c_str()) == 0);
    assert(strcmp(globalConfig.http.pass, "password-record") == 0);
    assert(strcmp(globalConfig.mqtt.server, "mqtt.lan") == 0);
}

int main()
{
    setup(); submit(false);
    for (auto change : {std::pair<const char *, const char *>{"static_ip", "192.168.1.200"},
            {"static_netmask", "255.255.254.0"}, {"static_gateway", "192.168.1.254"}, {"dhcp", "1"}})
    {
        setup(); endpoint.args[change.first] = change.second; submit(true);
    }
    setup(false); submit(false);
    setup(false); endpoint.args["dhcp"] = "2"; submit(true);
    setup(false); endpoint.args["static_ip"] = "192.168.1.200"; submit(false);
    setup();
    endpoint.args["static_ip"] = "192.168.100.200";
    endpoint.args["static_netmask"] = "255.255.255.128";
    endpoint.args["static_gateway"] = "192.168.100.129";
    submit(true);

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
    puts("PASS: actual network handler detects IP/mask/gateway/mode changes, inactive DHCP fields, full IPv4 lengths, validation/auth/save failures and unrelated settings");
}
