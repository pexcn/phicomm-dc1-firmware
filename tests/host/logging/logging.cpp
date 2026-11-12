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
    std::string data;
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

int main()
{
    globalConfig.debug.type = 4;
    Rtc::rtcTime.valid = true;
    char *entry; uint16_t len;
    for (unsigned i = 0; i < 1000; ++i) Debug::AddInfo(PSTR("SYS   Event %u"), i);
    assert(strlen(Debug::webLog) < WEB_LOG_SIZE);
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
    assert(entry && len > 511 && len < WEB_LOG_SIZE && strlen(Debug::webLog) < WEB_LOG_SIZE);
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
    assert(entry && std::string(entry, len - 1).find("SYS   Recovered") != std::string::npos);
    puts("PASS: log buffer eviction/index wrap, long records, streamed JSON escaping and damaged-entry recovery");
}
