#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "GlobalConfig.pb.h"

#define F(text) text
#define PSTR(text) text
#define WIFI_AP_STA 3
#define WIFI_CONNECT_TIMEOUT 300
#define WIFI_PORTAL_TIMEOUT 300
class String : public std::string {
public:
    using std::string::string;
    String(const std::string &value) : std::string(value) {}
};
struct IPAddress { String toString() const { return "192.168.4.1"; } };
struct WiFiClient {};
struct WiFiEventStationModeGotIP {};
using WiFiEventHandler = unsigned;
static unsigned long now = 1000;
unsigned long millis() { return now; }
void delay(unsigned long ms) { now += ms; }
static GlobalConfigMessage globalConfig;
static char UID[] = "dc1_abcdef";
struct Debug {
    static void AddInfo(const char *, ...) {}
    static void AddError(const char *, ...) {}
};
struct WiFiSpy {
    bool connected = false;
    unsigned apStarts = 0, connectionAttempts = 0;
    template<class Callback> WiFiEventHandler onStationModeGotIP(Callback) { return 1; }
    void hostname(const char *value) { assert(strcmp(value, UID) == 0); }
    bool isConnected() const { return connected; }
    void mode(int value) { assert(value == WIFI_AP_STA); }
    void persistent(bool) {}
    void disconnect(bool = false) { connected = false; }
    void softAP(const char *value) { assert(strcmp(value, UID) == 0); ++apStarts; }
    IPAddress softAPIP() const { return {}; }
    IPAddress localIP() const { return {}; }
    String SSID() const { return "new-home"; }
    void begin(const char *ssid, const char *password)
    {
        assert(strcmp(ssid, "new-home") == 0 && strcmp(password, "new-wifi-password") == 0);
        ++connectionAttempts;
    }
} WiFi;
enum class DNSReplyCode { NoError };
static unsigned dnsObjects, dnsStarts, dnsStops, dnsRequests, dnsSockets;
struct DNSServer {
    bool active = false;
    DNSServer() { ++dnsObjects; }
    void setErrorReplyCode(DNSReplyCode code) { assert(code == DNSReplyCode::NoError); }
    void start(unsigned port, const char *domain, IPAddress)
    {
        assert(port == 53 && strcmp(domain, "*") == 0 && !active);
        active = true; ++dnsStarts; ++dnsSockets;
    }
    void stop() { ++dnsStops; if (active) { active = false; --dnsSockets; } }
    void processNextRequest() { assert(active); ++dnsRequests; }
};
static bool saveOk = false;
static unsigned saves, resets, restartPreparations, restarts;
struct Config {
    static void resetConfig() { ++resets; }
    static bool saveConfig() { ++saves; return saveOk; }
    static bool prepareRestart() { ++restartPreparations; return saveOk; }
};
struct ESPSpy { void restart() { ++restarts; } } ESP;
struct Wifi {
    static WiFiClient wifiClient;
    static WiFiEventHandler STAGotIP;
    static unsigned long configPortalStart, connectStart;
    static bool connect;
    static String _ssid, _pass;
    static DNSServer *dnsServer;
    static void setupWifiManager(bool resetSettings);
    static void tryConnect(String ssid, String pass);
    static void loop();
};
#include "wifi-portal.inc"

int main()
{
    strcpy(globalConfig.wifi.ssid, "saved-home");
    strcpy(globalConfig.wifi.pass, "saved-wifi-password");
    strcpy(globalConfig.http.pass, "saved-admin-password-record");
    strcpy(globalConfig.mqtt.server, "mqtt.lan");
    const auto original = globalConfig;
    // Repeated button presses while the portal is already active must reuse DNS.
    for (unsigned i = 0; i < 100; ++i)
    {
        Wifi::setupWifiManager(false);
        assert(dnsObjects == 1 && dnsSockets == 1 && dnsStarts == i + 1);
        assert(resets == 0 && saves == 0);
        Wifi::loop();
        assert(dnsRequests == i + 1);
    }
    assert(dnsStops == 99);
    assert(memcmp(&globalConfig, &original, sizeof(original)) == 0);
    auto *const reused = Wifi::dnsServer;

    // Timeout stops DNS. A cancelled restart must still permit a later reopen.
    now += WIFI_PORTAL_TIMEOUT * 1000UL;
    Wifi::loop();
    assert(dnsSockets == 0 && Wifi::configPortalStart == 0);
    now += 3000; Wifi::loop();
    assert(restartPreparations == 1 && restarts == 0);
    const unsigned requests = dnsRequests;
    Wifi::loop(); assert(dnsRequests == requests);
    Wifi::setupWifiManager(false);
    assert(Wifi::dnsServer == reused && dnsObjects == 1 && dnsSockets == 1);

    // The automatic connection-timeout entry point must also reuse the object.
    Wifi::connectStart = now - WIFI_CONNECT_TIMEOUT * 1000UL - 1;
    Wifi::loop();
    assert(Wifi::dnsServer == reused && dnsObjects == 1 && dnsSockets == 1);
    assert(resets == 0 && memcmp(&globalConfig, &original, sizeof(original)) == 0);

    Wifi::tryConnect("new-home", "new-wifi-password");
    Wifi::loop();
    assert(WiFi.connectionAttempts == 1 && saves == 0);
    WiFi.connected = true;
    Wifi::loop();
    assert(saves == 1 && dnsSockets == 1 && Wifi::configPortalStart != 0);
    // Saving failure keeps the portal and DNS running until the retry succeeds.
    saveOk = true;
    now += 60000; Wifi::loop();
    assert(saves == 2 && dnsSockets == 0 && Wifi::configPortalStart == 0);
    assert(Wifi::dnsServer == reused && dnsObjects == 1 && restarts == 0);
    assert(strcmp(globalConfig.wifi.ssid, "new-home") == 0);
    assert(strcmp(globalConfig.wifi.pass, "new-wifi-password") == 0);
    assert(memcmp(&globalConfig.http, &original.http, sizeof(original.http)) == 0);
    assert(memcmp(&globalConfig.mqtt, &original.mqtt, sizeof(original.mqtt)) == 0);
    now += 3000; Wifi::loop();
    assert(restarts == 1 && restartPreparations == 2);
    // Reopening while already connected follows the other AP setup branch.
    Wifi::setupWifiManager(false);
    assert(Wifi::dnsServer == reused && dnsObjects == 1 && dnsSockets == 1);
    assert(resets == 0);
    puts("PASS: actual Wi-Fi portal reuses one DNS object across repeated opens, timeouts, save failures/success, and reconnects; configuration preserved");
}
