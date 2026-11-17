#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include "DC1Config.pb.h"
#include "NightLight.h"

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
struct { struct { unsigned port = 1883; } mqtt; } globalConfig;
struct Mqtt { struct Client { bool connected() { return false; } }; static Client mqttClient; };
Mqtt::Client Mqtt::mqttClient;
struct Wifi { static unsigned configPortalStart; };
unsigned Wifi::configPortalStart;
struct Rtc {
    static uint32_t utcTime;
    struct Time { uint16_t year = 2026, day_of_year = 0; bool valid = false; };
    static Time rtcTime;
};
uint32_t Rtc::utcTime;
Rtc::Time Rtc::rtcTime;
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
        assert(std::abs(NightLight::sunrise(sample.year, sample.doy, sample.lat, sample.lon) - sample.localMinute) <= 5);
    assert(NightLight::sunrise(2026, 171, 800000, 0) == -1);
    assert(NightLight::sunrise(2026, 354, 800000, 0) == -1);
    uint16_t off;
    int32_t coordinate;
    assert(NightLight::offTime("00:00", off) && off == 0);
    assert(!NightLight::offTime("12:60", off));
    assert(NightLight::coordinate("-0.0001", 900000, coordinate) && coordinate == -1);
    for (const char *bad : {"", "nan", "90.0001", "1.23456", "1x", "--1", "999999999999"})
        assert(!NightLight::coordinate(bad, 900000, coordinate));
    NightLight night;
    auto at = [&](unsigned day, unsigned minute, unsigned year = 2026, unsigned offMinute = 1380) {
        night.update(utc(day, minute, year), year, day, 302741, 1201551, offMinute, true);
    };
    const int rise = NightLight::sunrise(2026, 319, 302741, 1201551);
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
    night.update(0, 2026, 319, 302741, 1201551, 1380, true);
    assert(!night.quiet && night.nextRise == -1);
    at(319, 1380);
    night.update(utc(319, 1380), 2026, 319, 800000, 0, 1380, true);
    assert(!night.quiet && night.nextRise == -1); // Coordinates changed on same day.

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
    puts("PASS: sunrise estimates, midnight/reboot/NTP/leap-year/polar schedules, and real LED timers/feedback/mode restoration");
}
