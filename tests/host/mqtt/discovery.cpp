#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "GlobalConfig.pb.h"

#define F(text) text
#define PSTR(text) text

class String : public std::string {
public:
    using std::string::string;
    using std::string::operator=;
    String(const std::string &value) : std::string(value) {}
    int indexOf(const char *value) const
    {
        auto pos = find(value);
        return pos == npos ? -1 : int(pos);
    }
    void replace(const std::string &from, const std::string &to)
    {
        size_t pos = 0;
        while ((pos = find(from, pos)) != npos)
        {
            std::string::replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    bool endsWith(const char *value) const
    {
        size_t length = strlen(value);
        return size() >= length && compare(size() - length, length, value) == 0;
    }
};

static GlobalConfigMessage globalConfig;
static char UID[16];
static unsigned errors, formatCalls, failFormatCall;
static bool formatError;
static int boundedFormat(char *buffer, size_t size, const char *format, ...)
{
    if (++formatCalls == failFormatCall)
    {
        buffer[0] = 0;
        return formatError ? -1 : int(size);
    }
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}
#define snprintf_P boundedFormat

struct Packet { std::string topic, message; bool retained; };
static std::vector<Packet> packets;
static unsigned availabilityReports, powerReports, energyReports;
struct Debug {
    static void AddError(const char *, ...) { ++errors; }
};
struct Module {
    String getModuleName() { return "dc1"; }
};
static Module device;
static Module *module = &device;
struct Mqtt {
    static String getTopic(uint8_t prefix, String subtopic);
    static String getCmndTopic(String topic);
    static String getStatTopic(String topic);
    static String getTeleTopic(String topic);
    static bool publish(const char *topic, const char *message, bool retained)
    {
        packets.push_back({topic, message, retained});
        return true;
    }
    static void availability() { ++availabilityReports; }
};
class DC1 {
public:
    char powerStatTopic[100];
    uint8_t channels = 4;
    void mqttDiscovery(bool isEnable);
    void reportPower() { ++powerReports; }
    void reportEnergy() { ++energyReports; }
};

#include "mqtt-topic.inc"
#include "mqtt-discovery.inc"

static void resetSpies()
{
    packets.clear();
    errors = formatCalls = failFormatCall = 0;
    availabilityReports = powerReports = energyReports = 0;
}

static void testConfiguration(const char *uid, const char *prefix, const char *fulltopic)
{
    assert(strlen(uid) < sizeof(UID));
    assert(strlen(prefix) < sizeof(globalConfig.mqtt.discovery_prefix));
    assert(strlen(fulltopic) < sizeof(globalConfig.mqtt.topic));
    strcpy(UID, uid);
    strcpy(globalConfig.mqtt.discovery_prefix, prefix);
    strcpy(globalConfig.mqtt.topic, fulltopic);
    DC1 dc;
    auto state = Mqtt::getStatTopic("power1");
    assert(state.size() < sizeof(dc.powerStatTopic));
    strcpy(dc.powerStatTopic, state.c_str());
    resetSpies();
    dc.mqttDiscovery(true);
    assert(errors == 0 && packets.size() == 14);
    assert(availabilityReports == 1 && powerReports == 1 && energyReports == 1);
    const auto enabled = packets;
    const char *sensors[] = {"voltage", "current", "power", "apparent_power", "reactive_power",
        "factor", "total", "yesterday", "today", "starttime"};
    for (unsigned i = 0; i < packets.size(); ++i)
    {
        const auto &packet = packets[i];
        auto entity = i < 4 ? std::to_string(i + 1) : sensors[i - 4];
        std::string expected = std::string(prefix) + (i < 4 ? "/switch/" : "/sensor/")
            + uid + "_" + entity + "/config";
        assert(packet.topic == expected && packet.retained && !packet.message.empty());
        // PubSubClient's configured packet size includes fixed and topic headers.
        assert(5 + 2 + packet.topic.size() + packet.message.size() <= 768);
        printf("PACKET\t%s\t%s\n", packet.topic.c_str(), packet.message.c_str());
    }
    resetSpies();
    dc.mqttDiscovery(false);
    assert(errors == 0 && packets.size() == enabled.size());
    assert(availabilityReports == 0 && powerReports == 0 && energyReports == 0);
    for (unsigned i = 0; i < packets.size(); ++i)
        assert(packets[i].topic == enabled[i].topic && packets[i].message.empty() && packets[i].retained);

    // Simulate snprintf failures at command/topic, switch JSON and both sensor JSON branches.
    for (unsigned failure : {1U, 2U, 3U, 10U, 11U, 21U})
    for (bool negative : {false, true})
    {
        resetSpies();
        failFormatCall = failure;
        formatError = negative;
        dc.mqttDiscovery(true);
        assert(errors == 1 && formatCalls == failure);
        unsigned expected = failure <= 3 ? 0 : failure <= 11 ? 4 : 9;
        assert(packets.size() == expected);
        assert(availabilityReports == 0 && powerReports == 0 && energyReports == 0);
        for (const auto &packet : packets) assert(!packet.message.empty());
    }
}

int main()
{
    testConfiguration("dc1_abcdef", "homeassistant", "%module%/%hostname%/%prefix%/");
    const std::string uid(15, 'd'), prefix(29, 'h');
    // Four expanded hostnames and no prefix produce the longest command path.
    const std::string expanded = "%hostname%%hostname%%hostname%%hostname%123456789";
    assert(expanded.size() == 49);
    testConfiguration(uid.c_str(), prefix.c_str(), expanded.c_str());
    testConfiguration(uid.c_str(), prefix.c_str(), std::string(49, 't').c_str());
    testConfiguration("d", "h", "%prefix%/");
    puts("PASS: actual MQTT Discovery default/max topics, JSON, retained removal and formatting failures");
}
