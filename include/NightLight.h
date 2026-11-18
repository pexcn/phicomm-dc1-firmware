#ifndef NIGHT_LIGHT_H
#define NIGHT_LIGHT_H

#include <stdint.h>

// All schedule times use the device's configured UTC offset.
class NightLight
{
    uint32_t cachedDay = UINT32_MAX;
    int32_t cachedLatitude = 0, cachedLongitude = 0;
    int16_t cachedOffset = 0;
    int16_t rises[3] = {-1, -1, -1}; // Yesterday, today, tomorrow.
public:
    bool quiet = false;
    int16_t nextRise = -1; // Minutes from today's local midnight (may exceed 1440).
    static int16_t sunrise(uint16_t year, uint16_t dayOfYear, int32_t latitude, int32_t longitude,
        int16_t offsetMinutes);
    static bool coordinate(const char *text, int32_t limit, int32_t &value);
    static bool offTime(const char *text, uint16_t &minute);
    void update(uint32_t utc, uint16_t year, uint16_t dayOfYear,
        int32_t latitude, int32_t longitude, uint16_t offMinute, bool enabled, int16_t offsetMinutes);
};

#endif
