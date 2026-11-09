#include "Config.h"
#include "Debug.h"
#include "Rtc.h"

#ifdef WEB_LOG_SIZE
uint8_t Debug::webLogIndex = 1;
char Debug::webLog[WEB_LOG_SIZE] = {'\0'};
#endif

size_t Debug::strchrspn(const char *str1, int character)
{
    const char *end = strchr(str1, character);
    return end ? size_t(end - str1) : 0;
}

#ifdef WEB_LOG_SIZE
void Debug::GetLog(uint8_t idx, char **entry_pp, uint16_t *len_p)
{
    char *entry_p = NULL;
    size_t len = 0;

    if (idx)
    {
        char *it = webLog;
        do
        {
            uint8_t cur_idx = *it;
            it++;
            size_t tmp = strchrspn(it, '\1');
            tmp++; // Skip terminating '\1'
            if (cur_idx == idx)
            { // Found the requested entry
                len = tmp;
                entry_p = it;
                break;
            }
            it += tmp;
        } while (it < webLog + WEB_LOG_SIZE && *it != '\0');
    }
    *entry_pp = entry_p;
    *len_p = len;
}
#endif


void Debug::AddLog(uint8_t loglevel)
{
    uint8_t outputs = globalConfig.debug.type & (1 | 8);
#ifdef WEB_LOG_SIZE
    if ((globalConfig.debug.type & 4) || loglevel == LOG_LEVEL_ERROR) outputs |= 4;
#endif
    if (!outputs) return;

    char prefix[20]; // Also fits an uncalibrated uptime of UINT32_MAX seconds.
    char level = loglevel == LOG_LEVEL_ERROR ? 'E' : loglevel >= LOG_LEVEL_DEBUG ? 'D' : 'I';
    size_t prefixLength;
    if (Rtc::rtcTime.valid)
    {
        // Fixed-width clock: avoid printf conversions on the normal logging path.
        memcpy_P(prefix, PSTR(" 00:00:00 [I] "), 15);
        prefix[1] += Rtc::rtcTime.hour / 10;
        prefix[2] += Rtc::rtcTime.hour % 10;
        prefix[4] += Rtc::rtcTime.minute / 10;
        prefix[5] += Rtc::rtcTime.minute % 10;
        prefix[7] += Rtc::rtcTime.second / 10;
        prefix[8] += Rtc::rtcTime.second % 10;
        prefix[11] = level;
        prefixLength = 14;
    }
    else
    {
        // Reuse the existing one-second counter; no clock queries or new timers.
        uint32_t seconds = perSecond;
        prefixLength = snprintf_P(prefix, sizeof(prefix), PSTR("+%02u:%02u:%02u [%c] "),
            unsigned(seconds / 3600), unsigned(seconds / 60 % 60), unsigned(seconds % 60), level);
    }
    size_t messageLength = strlen(tmpData);
    if (outputs & 1)
    {
        Serial.write(reinterpret_cast<const uint8_t *>(prefix), prefixLength);
        Serial.write(reinterpret_cast<const uint8_t *>(tmpData), messageLength);
        Serial.write(reinterpret_cast<const uint8_t *>("\r\n"), 2);
    }
    if (outputs & 8)
    {
        Serial1.write(reinterpret_cast<const uint8_t *>(prefix), prefixLength);
        Serial1.write(reinterpret_cast<const uint8_t *>(tmpData), messageLength);
        Serial1.write(reinterpret_cast<const uint8_t *>("\r\n"), 2);
    }

#ifdef WEB_LOG_SIZE
    // Delimited, zero-terminated buffer of log lines.
    // Each entry has this format: [index][log data]['\1']
    if (outputs & 4)
    {
        size_t needed = prefixLength + messageLength + 3; // Index, delimiter and NUL.
        if (needed > sizeof(webLog)) return;
        size_t used = strlen(webLog);
        if (!webLogIndex) ++webLogIndex;
        while (used && (webLogIndex == uint8_t(webLog[0]) || used + needed > sizeof(webLog)))
        {
            size_t removed = strchrspn(webLog + 1, '\1') + 2;
            if (removed > used)
            {
                used = 0;
                break;
            }
            memmove(webLog, webLog + removed, used - removed + 1);
            used -= removed;
        }
        // Append only the new entry; never reformat or copy the existing log.
        char *entry = webLog + used;
        *entry++ = webLogIndex++;
        memcpy(entry, prefix, prefixLength);
        entry += prefixLength;
        memcpy(entry, tmpData, messageLength);
        entry[messageLength] = '\1';
        entry[messageLength + 1] = '\0';
        if (!webLogIndex) ++webLogIndex;
    }
#endif

}

void Debug::AddLog(uint8_t loglevel, PGM_P formatP, ...)
{
    va_list arg;
    va_start(arg, formatP);
    vsnprintf_P(tmpData, sizeof(tmpData), formatP, arg);
    va_end(arg);

    AddLog(loglevel);
}

void Debug::AddInfo(PGM_P formatP, ...)
{
    va_list arg;
    va_start(arg, formatP);
    vsnprintf_P(tmpData, sizeof(tmpData), formatP, arg);
    va_end(arg);

    AddLog(LOG_LEVEL_INFO);
}

void Debug::AddDebug(PGM_P formatP, ...)
{
    va_list arg;
    va_start(arg, formatP);
    vsnprintf_P(tmpData, sizeof(tmpData), formatP, arg);
    va_end(arg);

    AddLog(LOG_LEVEL_DEBUG);
}

void Debug::AddError(PGM_P formatP, ...)
{
    va_list arg;
    va_start(arg, formatP);
    vsnprintf_P(tmpData, sizeof(tmpData), formatP, arg);
    va_end(arg);

    AddLog(LOG_LEVEL_ERROR);
}
