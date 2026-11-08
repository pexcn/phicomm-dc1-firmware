#ifndef ENERGY_STATE_H
#define ENERGY_STATE_H

#include <stdint.h>

// Runtime snapshot: stored separately from the device configuration.
struct EnergyState
{
    uint64_t today = 0;
    uint64_t yesterday = 0;
    uint64_t total = 0;
    uint32_t day = 0;
    uint32_t startTime = 0;
    bool utcDay = false; // Version 1 snapshots used UTC calendar days.
    uint32_t legacyDay = UINT32_MAX; // Old firmware stored only day-of-year.

    void resolveLegacyDay(uint32_t currentDay, uint16_t dayOfYear, uint16_t previousYearDays)
    {
        if (legacyDay == UINT32_MAX) return;
        uint32_t elapsed = dayOfYear >= legacyDay ? dayOfYear - legacyDay
            : dayOfYear + previousYearDays - legacyDay;
        day = currentDay - elapsed;
        legacyDay = UINT32_MAX;
    }

    void useLocalDay(uint32_t currentUtcDay, uint32_t currentLocalDay)
    {
        if (!utcDay) return;
        // Historical hourly usage cannot be reconstructed. Keep the counters and
        // their age during the one-time switch; future rollovers use UTC+8.
        if (day != 0) day += currentLocalDay - currentUtcDay;
        utcDay = false;
    }

    void add(uint32_t amount)
    {
        today += amount;
        total += amount;
    }

    void updateDay(uint32_t currentDay)
    {
        // The first NTP sync assigns a date without discarding pre-sync usage.
        if (day != 0 && currentDay <= day) return; // Do not reset on a clock rollback.
        if (day != 0 && day != currentDay)
        {
            yesterday = currentDay == day + 1 ? today : 0;
            today = 0;
        }
        day = currentDay;
    }
};

#endif
