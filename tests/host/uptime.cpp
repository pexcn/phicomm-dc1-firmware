#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#define F(text) text
#define PSTR(text) text
#define snprintf_P snprintf
class String : public std::string {
public:
    using std::string::string;
    String(const std::string &value) : std::string(value) {}
    String(int value) : std::string(std::to_string(value)) {}
};
static uint64_t clockUs;
uint64_t micros64() { return clockUs; }
unsigned long millis() { return uint32_t(clockUs / 1000); }
struct Rtc { static String uptime(); };
struct IPAddress { String toString() const { return "192.168.100.200"; } };
struct WiFiSpy {
    String SSID() const { return std::string(32, 's'); }
    int RSSI() const { return -54; }
    IPAddress localIP() const { return {}; }
    String macAddress() const { return "AA:BB:CC:DD:EE:FF"; }
} WiFi;
struct ESPSpy { int getFreeHeap() const { return 40000; } } ESP;
struct Module { String getModuleVersion() const { return std::string(23, 'v'); } } device;
static Module *module = &device;
static char UID[] = "dc1_123456789ab";
static std::string message;
struct Mqtt {
    static void doReportHeartbeat();
    static String getTeleTopic(String leaf) { return "dc1/device/tele/" + leaf; }
    static bool publish(String topic, const char *payload)
    {
        assert(topic == "dc1/device/tele/HEARTBEAT");
        message = payload;
        return true;
    }
};
#include "uptime-rtc.inc"
#include "uptime-heartbeat.inc"
static std::string statusFields()
{
    char tmpData[512];
#include "uptime-status.inc"
    return "{" + std::string(tmpData) + "}";
}

int main()
{
    const uint64_t day = 86400000, wrap = uint64_t(UINT32_MAX) + 1;
    struct Sample { uint64_t milliseconds; const char *human; };
    const Sample samples[] = {
        {0, "0T00:00:00"}, {999, "0T00:00:00"}, {1000, "0T00:00:01"},
        {60000, "0T00:01:00"}, {day, "1T00:00:00"},
        {wrap - 1, "49T17:02:47"}, {wrap, "49T17:02:47"}, {wrap + 1000, "49T17:02:48"},
        {255 * day, "255T00:00:00"}, {256 * day, "256T00:00:00"},
        {1000 * day + 45296000, "1000T12:34:56"},
        {(uint64_t(INT32_MAX) + 1) * 1000, "24855T03:14:08"},
        {uint64_t(UINT32_MAX) * 1000 + 999, "49710T06:28:15"}
    };
    for (const auto &sample : samples)
    {
        clockUs = sample.milliseconds * 1000;
        assert(Rtc::uptime() == sample.human);
        Mqtt::doReportHeartbeat();
        const auto fields = statusFields();
        assert(fields.find(std::string("\"uptime\":\"") + sample.human + "\"") != std::string::npos);
        const auto seconds = unsigned(sample.milliseconds / 1000);
        assert(message.find("\"uptime\":" + std::to_string(seconds) + "}") != std::string::npos);
        printf("UPTIME\t%u\t%s\t%s\n", seconds, message.c_str(), fields.c_str());
    }
    puts("PASS: actual WebUI status and MQTT heartbeat uptime span millis rollover, 256/1000 days and unsigned 32-bit seconds without extra state");
}
