#include "NightLight.h"
#include <math.h>
#include <string.h>

namespace {
bool leap(uint16_t year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
}

int16_t NightLight::sunrise(uint16_t year, uint16_t dayOfYear, int32_t latitude, int32_t longitude,
    int16_t offsetMinutes)
{
    // NOAA fractional-year approximation; dayOfYear is zero-based.
    // Standard sunrise: solar centre 0.833 degrees below the horizon.
    const float rad = 0.01745329252f;
    float gamma = 6.28318530718f * dayOfYear / (leap(year) ? 366 : 365);
    float equation = 229.18f * (0.000075f + 0.001868f * cosf(gamma)
        - 0.032077f * sinf(gamma) - 0.014615f * cosf(2 * gamma)
        - 0.040849f * sinf(2 * gamma));
    float declination = 0.006918f - 0.399912f * cosf(gamma) + 0.070257f * sinf(gamma)
        - 0.006758f * cosf(2 * gamma) + 0.000907f * sinf(2 * gamma)
        - 0.002697f * cosf(3 * gamma) + 0.00148f * sinf(3 * gamma);
    float lat = latitude * (rad / 10000);
    float denominator = cosf(lat) * cosf(declination);
    if (fabsf(denominator) < 0.000001f) return -1;
    float angle = (cosf(90.833f * rad) - sinf(lat) * sinf(declination)) / denominator;
    if (angle < -1 || angle > 1) return -1; // Polar day/night: no sunrise.
    float minute = 720 - 4 * (longitude / 10000.0f + acosf(angle) / rad) - equation + offsetMinutes;
    int result = int(roundf(minute)) % 1440;
    return result < 0 ? result + 1440 : result;
}

bool NightLight::coordinate(const char *text, int32_t limit, int32_t &value)
{
    bool negative = *text == '-';
    if (negative) ++text;
    if (*text < '0' || *text > '9') return false;
    int32_t whole = 0, fraction = 0, scale = 1000;
    unsigned digits = 0;
    while (*text >= '0' && *text <= '9')
    {
        if (++digits > 3) return false;
        whole = whole * 10 + *text++ - '0';
    }
    if (*text == '.')
    {
        ++text;
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9')
        {
            if (scale == 0) return false;
            fraction += (*text++ - '0') * scale;
            scale /= 10;
        }
    }
    if (*text || whole * 10000 + fraction > limit) return false;
    value = (whole * 10000 + fraction) * (negative ? -1 : 1);
    return true;
}

bool NightLight::offTime(const char *text, uint16_t &minute)
{
    if (strlen(text) != 5 || text[2] != ':') return false;
    for (unsigned i = 0; i < 5; ++i)
        if (i != 2 && (text[i] < '0' || text[i] > '9')) return false;
    unsigned hour = (text[0] - '0') * 10 + text[1] - '0';
    unsigned part = (text[3] - '0') * 10 + text[4] - '0';
    if (hour > 23 || part > 59) return false;
    minute = hour * 60 + part;
    return true;
}

void NightLight::update(uint32_t utc, uint16_t year, uint16_t dayOfYear,
    int32_t latitude, int32_t longitude, uint16_t offMinute, bool enabled, int16_t offsetMinutes)
{
    quiet = false;
    nextRise = -1;
    if (!enabled || utc == 0 || offMinute >= 1440
        || latitude < -900000 || latitude > 900000
        || longitude < -1800000 || longitude > 1800000) return;
    uint32_t local = utc + int32_t(offsetMinutes) * 60;
    uint32_t day = local / 86400;
    uint16_t minute = local % 86400 / 60;
    if (cachedDay != day || cachedLatitude != latitude || cachedLongitude != longitude
        || cachedOffset != offsetMinutes)
    {
        cachedDay = day;
        cachedLatitude = latitude;
        cachedLongitude = longitude;
        cachedOffset = offsetMinutes;
        rises[0] = sunrise(dayOfYear ? year : year - 1,
            dayOfYear ? dayOfYear - 1 : (leap(year - 1) ? 365 : 364), latitude, longitude, offsetMinutes);
        rises[1] = sunrise(year, dayOfYear, latitude, longitude, offsetMinutes);
        bool lastDay = dayOfYear == (leap(year) ? 365 : 364);
        rises[2] = sunrise(lastDay ? year + 1 : year, lastDay ? 0 : dayOfYear + 1, latitude, longitude, offsetMinutes);
    }
    if (rises[1] < 0) return; // Do not leave LEDs dark indefinitely without a sunrise.
    nextRise = minute < rises[1] ? rises[1] : rises[2] < 0 ? -1 : rises[2] + 1440;
    // Use the most recent bedtime and its first following sunrise, including
    // a bedtime before dawn. Recomputing from time also handles reboot/NTP jumps.
    if (minute >= offMinute)
        quiet = rises[1] > offMinute ? minute < rises[1] : rises[2] >= 0;
    else
        quiet = rises[0] >= 0 && rises[0] <= offMinute && minute < rises[1];
}
