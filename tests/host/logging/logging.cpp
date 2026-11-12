#include "Config.h"
#include "Debug.h"
#include "Rtc.h"
#include <cassert>
#include <string>

LogConfig globalConfig;
char tmpData[512];
uint32_t perSecond;
SerialSink Serial, Serial1;
LogClock Rtc::rtcTime;

struct Response {
    struct Argument {
        uint8_t index;
        int toInt() const { return index; }
    };
    std::string data;
    uint8_t requestedIndex = 0;
    Argument arg(const char *) const { return {requestedIndex}; }
    void sendContent_P(const char *bytes) { data += bytes; }
    void sendContent(const char *bytes, size_t size) { data.append(bytes, size); }
};
static std::string escapeLog(char *tmp, uint16_t len)
{
    Response response;
    Response *server = &response;
#include "log-json.inc"
    return response.data;
}

static std::string logBatch(uint8_t requestedIndex)
{
    Response response;
    response.requestedIndex = requestedIndex;
    Response *server = &response;
#include "log-batch.inc"
    const std::string prefix = ",\"logindex\":" + std::to_string(Debug::webLogIndex) + ",\"log\":\"";
    assert(response.data.compare(0, prefix.size(), prefix) == 0);
    assert(response.data.back() == '"');
    return response.data.substr(prefix.size(), response.data.size() - prefix.size() - 1);
}

int main()
{
    // An HTTP batch has separators only between records, including incremental reads.
    globalConfig.debug.type = 4;
    Rtc::rtcTime.valid = true;
    assert(logBatch(0).empty() && logBatch(Debug::webLogIndex).empty());
    Debug::AddInfo(PSTR("RELAY Outlet 3=ON"));
    const std::string on = " 00:00:00 [I] RELAY Outlet 3=ON";
    assert(logBatch(0) == on);
    uint8_t next = Debug::webLogIndex;
    assert(logBatch(next).empty());
    Debug::AddInfo(PSTR("RELAY Outlet 3=OFF"));
    const std::string off = " 00:00:00 [I] RELAY Outlet 3=OFF";
    assert(logBatch(next) == off);
    assert(logBatch(0) == on + "\\n" + off);
    // A stale cursor skips evicted IDs without adding an empty first line.
    Debug::webLog[0] = 0; Debug::webLogIndex = 255;
    Debug::AddInfo(PSTR("RELAY Outlet 3=ON"));
    Debug::AddInfo(PSTR("RELAY Outlet 3=OFF"));
    assert(Debug::webLogIndex == 2);
    assert(logBatch(254) == on + "\\n" + off);
    assert(logBatch(1) == off && logBatch(2).empty());
    Debug::webLog[0] = 0; Debug::webLogIndex = 1;

    globalConfig.debug.type = 13;
    Rtc::rtcTime.hour = 17; Rtc::rtcTime.minute = 54; Rtc::rtcTime.second = 22;
    Rtc::rtcTime.valid = true;
    Debug::AddInfo(PSTR("WIFI  Connected"));
    assert(strcmp(Serial.data, " 17:54:22 [I] WIFI  Connected\r\n") == 0);
    assert(strcmp(Serial.data, Serial1.data) == 0);
    char *entry; uint16_t len;
    Debug::GetLog(1, &entry, &len);
    assert(std::string(entry, len - 1) == " 17:54:22 [I] WIFI  Connected");

    Rtc::rtcTime.valid = false;
    perSecond = 90061;
    Debug::AddError(PSTR("NTP   Waiting"));
    Debug::GetLog(2, &entry, &len);
    assert(std::string(entry, len - 1) == "+25:01:01 [E] NTP   Waiting");
    perSecond = UINT32_MAX;
    Debug::AddDebug(PSTR("SYS   Diagnostic"));
    Debug::GetLog(3, &entry, &len);
    assert(std::string(entry, len - 1) == "+1193046:28:15 [D] SYS   Diagnostic");

    globalConfig.debug.type = 0;
    unsigned index = Debug::webLogIndex;
    size_t serialBytes = Serial.used;
    Debug::AddInfo(PSTR("not routed"));
    assert(index == Debug::webLogIndex && Serial.used == serialBytes);
    Debug::AddError(PSTR("CFG   Error"));
    assert(Debug::webLogIndex == index + 1 && Serial.used == serialBytes);

    globalConfig.debug.type = 4;
    Rtc::rtcTime.valid = true;
    for (unsigned i = 0; i < 1000; ++i) Debug::AddInfo(PSTR("SYS   Event %u"), i);
    assert(strlen(Debug::webLog) < WEB_LOG_SIZE);
    Debug::GetLog(1, &entry, &len); // Evicted or reused IDs must never duplicate.
    bool seen[256] = {};
    for (char *p = Debug::webLog; *p; )
    {
        uint8_t id = *p++;
        assert(!seen[id] && id != 0); seen[id] = true;
        const char *end = strchr(p, '\1'); assert(end);
        Debug::GetLog(id, &entry, &len);
        assert(entry == p && len == size_t(end - p + 1));
        p += len;
    }
    Debug::GetLog(Debug::webLogIndex, &entry, &len); assert(!entry && len == 0);

    Debug::AddInfo(PSTR("%0600u"), 1U); // vsnprintf truncates the body to 511 bytes.
    uint8_t last = Debug::webLogIndex == 1 ? 255 : Debug::webLogIndex - 1;
    Debug::GetLog(last, &entry, &len);
    assert(len == 14 + 511 + 1 && strlen(Debug::webLog) < WEB_LOG_SIZE);
    std::string line(entry, len - 1);
    assert(escapeLog(entry, len) == line);
    std::string special(511, '"'); special += "\\\b\f\n\r\t"; special += '\1';
    std::string expected;
    expected.reserve(1034);
    for (unsigned i = 0; i < 511; ++i) expected += "\\\"";
    expected += "\\\\\\b\\f\\n\\r\\t";
    assert(escapeLog(&special[0], special.size()) == expected);

    // Corrupt/partial old entries must not underflow the eviction length.
    Debug::webLog[0] = 1; Debug::webLog[1] = 0; Debug::webLogIndex = 1;
    Debug::AddInfo(PSTR("SYS   Recovered"));
    Debug::GetLog(1, &entry, &len);
    assert(std::string(entry, len - 1) == " 17:54:22 [I] SYS   Recovered");
    puts("PASS: actual logger: HTTP batch separators, levels, clock/uptime, output routing, 4KB eviction, index wrap and long JSON lines");
}
