#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#define HIGH 1
#define LOW 0
#define LOGO_LED_PIN 14
#define bitRead(v, b) (((v) >> (b)) & 1)
#define bitSet(v, b) ((v) |= 1U << (b))
#define bitClear(v, b) ((v) &= ~(1U << (b)))

static uint32_t now;
static int keys[4] = {1, 1, 1, 1};
static unsigned resets, portals, switches;
static bool saveOk = true;
uint32_t millis() { return now; }
int digitalRead(unsigned pin) { assert(pin == 16); return keys[0]; }
void digitalWrite(unsigned pin, int) { assert(pin == LOGO_LED_PIN); }
class Http {
public:
    static bool resetPassword() { ++resets; return saveOk; }
};
class Wifi {
public:
    static void setupWifiManager(bool reset) { assert(!reset); ++portals; }
};
class Expander {
public:
    int digitalRead(unsigned pin) { assert(pin < 3); return keys[pin + 1]; }
};
class DC1 {
public:
    uint8_t btnGPIO[4] = {16, 0, 1, 2};
    uint8_t lastState = 11;
    Expander pins;
    Expander *cat9554 = &pins;
#include "button-fields.inc"
    void checkButton(uint8_t ch);
    void switchRelay(uint8_t ch, bool value, bool save)
    {
        assert(save);
        ++switches;
        if (value) bitSet(lastState, ch);
        else bitClear(lastState, ch);
    }
};
#include "button-handler.inc"

int main()
{
    for (unsigned ch = 0; ch < 4; ++ch)
    for (uint32_t start : {0U, UINT32_MAX - 5000})
    for (uint32_t held : {49U, 50U, 1999U, 2000U, 9999U, 10000U, 12000U})
    {
        DC1 dc;
        resets = portals = switches = 0;
        now = start; keys[ch] = 0; dc.checkButton(ch);
        now = start + held; dc.checkButton(ch);
        assert(resets == 0 && portals == 0 && switches == 0 && dc.lastState == 11);
        keys[ch] = 1; dc.checkButton(ch);
        assert(resets == unsigned(ch == 0 && held >= 10000));
        assert(portals == unsigned(ch == 0 && held >= 2000 && held < 10000));
        assert(switches == unsigned(held >= 50 && held < 2000));
        if (held >= 2000) assert(dc.lastState == 11);
        if (resets) assert(dc.passwordFeedbackPeriod == 100);
        unsigned count = resets + portals + switches;
        dc.checkButton(ch); dc.checkButton(ch);
        assert(resets + portals + switches == count);
    }
    DC1 dc;
    resets = portals = switches = 0;
    now = 0; keys[0] = 0; dc.checkButton(0);
    now = 9999; dc.checkButton(0);
    now = 12000; keys[0] = 1; dc.checkButton(0);
    assert(resets == 0 && portals == 1); // No sampled 10-second hold, no recovery.

    now = 20000; keys[0] = 0; dc.checkButton(0);
    now = 32000; keys[0] = 1; dc.checkButton(0);
    assert(resets == 0); // A stalled loop cannot turn a release into recovery.

    saveOk = false;
    now = 40000; keys[0] = 0; dc.checkButton(0);
    now = 50000; dc.checkButton(0); keys[0] = 1; dc.checkButton(0);
    assert(resets == 1 && dc.passwordFeedbackPeriod == 400 && dc.lastState == 11);
    puts("PASS: actual button handler: thresholds, release-only actions, delayed polling, wrap and failure feedback");
}
