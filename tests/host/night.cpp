#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include "GlobalConfig.pb.h"
#include "EnergyState.h"
#include "DC1Config.pb.h"
#include "NightLight.h"

#define PSTR(text) text
#define snprintf_P snprintf
class String : public std::string { public: using std::string::string; };
#define OUTPUT 1
#define HIGH 1
#define LOW 0
#define WL_CONNECTED 3
#define LOGO_LED_PIN 14
#define bitRead(v, b) (((v) >> (b)) & 1)
static int gpio[16];
void pinMode(unsigned, int) {}
void digitalWrite(unsigned pin, int value) { gpio[pin] = value; }
void delay(int) {}
class Ticker {
public:
    std::function<void()> callback;
    bool active() const { return bool(callback); }
    void detach() { callback = {}; }
    void attach(float, std::function<void()> fn) { callback = fn; }
    void attach(int, void (*fn)(int), int arg) { callback = [=] { fn(arg); }; }
    void once_ms(int, std::function<void()> fn) { callback = fn; }
};
#include "Led.h"
struct TestLed : Led { using Led::ledTicker; using Led::ledTicker2; };
struct { int state = WL_CONNECTED; int status() const { return state; } } WiFi;
GlobalConfigMessage globalConfig = {};
struct Mqtt { struct Client { bool connected() { return false; } }; static Client mqttClient; };
Mqtt::Client Mqtt::mqttClient;
struct Wifi { static unsigned configPortalStart; };
unsigned Wifi::configPortalStart;
#include "Rtc.h"
uint32_t Rtc::utcTime;
TIME_T Rtc::rtcTime;
static int32_t sdkOffset;
void sntp_set_timezone_in_seconds(int32_t value) { sdkOffset = value; }
#include "rtc-calendar.inc"
class DC1 {
public:
    NightLight nightLight;
    DC1ConfigMessage config = {};
    uint8_t lastState = 0;
    uint16_t passwordFeedbackPeriod = 0;
    void updateNightLight();
    void applyNightLight();
    void logoLed();
    bool moduleLed();
};
DC1 *module;
#include "led.inc"
#include "night-led.inc"

static uint32_t utc(unsigned doy, unsigned minute, unsigned year = 2026)
{
    // 2026-01-01 and 2028-01-01 UTC; local midnight is eight hours earlier.
    return (year == 2026 ? 1767225600U : 1830297600U) + doy * 86400 + minute * 60 - 8 * 3600;
}

int main()
{
    // Published sunrise times rounded to minutes; allow the NOAA approximation
    // five minutes (flat horizon, standard refraction, no terrain correction).
    struct Example { unsigned year, doy; int32_t lat, lon; int localMinute; };
    for (const auto &sample : {Example{2026, 171, 399042, 1164074, 4*60+46},
            Example{2026, 354, 399042, 1164074, 7*60+33},
            Example{2026, 354, -338688, 1512093, 2*60+41},
            Example{2026, 171, 515074, -1278, 11*60+43}})
        assert(std::abs(NightLight::sunrise(sample.year, sample.doy, sample.lat, sample.lon, 480) - sample.localMinute) <= 5);
    assert(NightLight::sunrise(2026, 171, 800000, 0, 480) == -1);
    assert(NightLight::sunrise(2026, 354, 800000, 0, 480) == -1);
    uint16_t off;
    int32_t coordinate;
    assert(NightLight::offTime("00:00", off) && off == 0);
    assert(!NightLight::offTime("12:60", off));
    assert(NightLight::coordinate("-0.0001", 900000, coordinate) && coordinate == -1);
    for (const char *bad : {"", "nan", "90.0001", "1.23456", "1x", "--1", "999999999999"})
        assert(!NightLight::coordinate(bad, 900000, coordinate));
    NightLight night;
    auto at = [&](unsigned day, unsigned minute, unsigned year = 2026, unsigned offMinute = 1380) {
        night.update(utc(day, minute, year), year, day, 302741, 1201551, offMinute, true, 480);
    };
    const int rise = NightLight::sunrise(2026, 319, 302741, 1201551, 480);
    at(319, 1379); assert(!night.quiet);
    at(319, 1380); assert(night.quiet && night.nextRise >= 1440);
    at(319, 0); assert(night.quiet); // Clock correction backwards/reboot.
    at(319, rise - 1); assert(night.quiet);
    at(319, rise); assert(!night.quiet);
    at(319, 4*60, 2026, 4*60); assert(night.quiet); // Before-dawn bedtime.
    at(319, rise, 2026, 4*60); assert(!night.quiet);
    at(319, 0, 2026, 4*60); assert(!night.quiet);
    for (unsigned day : {0U, 58U, 59U, 60U, 365U}) {
        at(day, 0, 2028); assert(night.quiet && night.nextRise >= 0);
    }
    night.update(0, 2026, 319, 302741, 1201551, 1380, true, 480);
    assert(!night.quiet && night.nextRise == -1);
    at(319, 1380);
    night.update(utc(319, 1380), 2026, 319, 800000, 0, 1380, true, 480);
    assert(!night.quiet && night.nextRise == -1); // Coordinates changed on same day.

    // A timezone change on the same calendar day must invalidate solar caches.
    night.update(utc(319, 720), 2026, 319, 302741, 1201551, 1380, true, 480);
    int next = night.nextRise;
    night.update(utc(319, 720), 2026, 319, 302741, 1201551, 1380, true, 345);
    assert(night.nextRise == next - 135);
    assert(NightLight::sunrise(2026, 171, 515074, -1278, 0) >= 3*60+38
        && NightLight::sunrise(2026, 171, 515074, -1278, 0) <= 3*60+48);
    TIME_T tm;
    assert(Rtc::offsetMinutes() == 480);
    globalConfig.wifi.has_utc_offset_minutes = true;
    globalConfig.wifi.utc_offset_minutes = -300;
    Rtc::breakTime(1767225600, tm);
    assert(tm.year == 2025 && tm.month == 12 && tm.day_of_month == 31 && tm.hour == 19);
    Rtc::breakTime(1767225600, tm, false);
    assert(tm.year == 2026 && tm.month == 1 && tm.day_of_month == 1 && tm.hour == 0);
    globalConfig.wifi.utc_offset_minutes = -30;
    Rtc::breakTime(1835482500, tm); // 2028-03-01 00:15 UTC.
    assert(tm.year == 2028 && tm.month == 2 && tm.day_of_month == 29 && tm.hour == 23 && tm.minute == 45);
    globalConfig.wifi.utc_offset_minutes = 345;
    Rtc::utcTime = 1767225600; Rtc::rtcTime.valid = true;
    Rtc::refreshLocalTime();
    assert(sdkOffset == 20700 && Rtc::utcTime == 1767225600);
    assert(Rtc::rtcTime.hour == 5 && Rtc::rtcTime.minute == 45 && Rtc::offsetLabel() == "UTC+05:45");
    globalConfig.wifi.utc_offset_minutes = -300;
    EnergyState usage;
    Rtc::breakTime(1767243599, tm); // 04:59:59 UTC, still the previous local day.
    usage.updateDay(tm.days); usage.add(123);
    Rtc::breakTime(1767243600, tm); // 05:00:00 UTC: local midnight.
    usage.updateDay(tm.days);
    assert(usage.today == 0 && usage.yesterday == 123 && usage.total == 123);
    Rtc::breakTime(1767243599, tm); usage.updateDay(tm.days);
    assert(usage.yesterday == 123 && usage.total == 123); // No duplicate rollback reset.
    globalConfig.wifi.has_utc_offset_minutes = false;
    globalConfig.wifi.utc_offset_minutes = 0;
    Rtc::rtcTime = {}; Rtc::rtcTime.year = 2026;
    globalConfig.mqtt.port = 1883;
    DC1 dc;
    module = &dc;
    Led::init(0, LOW);
    assert(gpio[0] == HIGH);
    dc.config.night_led_enabled = true;
    dc.config.has_night_latitude = dc.config.has_night_longitude = true;
    dc.config.night_latitude = 302741;
    dc.config.night_longitude = 1201551;
    Rtc::utcTime = utc(319, 1380); Rtc::rtcTime.day_of_year = 319;
    dc.updateNightLight(); assert(!Led::isQuiet()); // No valid clock yet.
    Led::loop(); assert(TestLed::ledTicker->active());
    Rtc::rtcTime.valid = true;
    dc.updateNightLight();
    assert(Led::isQuiet() && gpio[0] == HIGH && gpio[14] == HIGH);
    assert(!TestLed::ledTicker->active() && !TestLed::ledTicker2->active());
    WiFi.state = 0;
    for (unsigned mode : {0U, 1U, 2U}) {
        dc.config.wifi_led = mode;
        Led::loop(); Led::on(); Led::toggle(); Led::led(); dc.logoLed();
        assert(gpio[0] == HIGH && gpio[14] == HIGH && !TestLed::ledTicker->active());
    }
    Led::blinkLED(1, 2); assert(Led::isQuiet()); // Explicit feedback restores quiet.
    Wifi::configPortalStart = 1; dc.applyNightLight(); Led::loop();
    assert(!Led::isQuiet() && TestLed::ledTicker->active());
    Wifi::configPortalStart = 0; dc.applyNightLight(); assert(Led::isQuiet());
    dc.passwordFeedbackPeriod = 100; gpio[14] = LOW; dc.logoLed();
    assert(gpio[14] == LOW); // Password feedback has priority.
    dc.passwordFeedbackPeriod = 0;
    Rtc::utcTime = utc(319, rise); dc.updateNightLight();
    assert(!Led::isQuiet());
    WiFi.state = WL_CONNECTED; globalConfig.mqtt.port = 0;
    for (unsigned mode : {0U, 1U, 2U, 3U}) {
        dc.config.logo_led = mode; dc.lastState = 1; dc.logoLed();
        assert(gpio[14] == (mode == 1 || mode == 3 ? HIGH : LOW));
    }
    dc.config.wifi_led = 1; Led::loop(); assert(gpio[0] == HIGH);
    Rtc::utcTime = utc(319, 1380); dc.updateNightLight(); assert(Led::isQuiet());
    dc.config.night_led_enabled = false; dc.updateNightLight();
    assert(!Led::isQuiet());
    delete TestLed::ledTicker; delete TestLed::ledTicker2;
    puts("PASS: UTC offsets, negative/fractional calendars, energy midnight, sunrise caches and schedules, and real LED timers/feedback/mode restoration");
}
