#include "CSE7766.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

volatile uint32_t gpioInput = 1UL << 13;
static uint64_t wireBits = 48;
static void (*rxHandler)(void *);
static void *rxArgument;

uint32_t micros() { return wireBits * 1000000 / 4800; }
unsigned long millis() { return micros() / 1000; }
void delay(unsigned long ms) { wireBits += ms * 4800 / 1000; }
void optimistic_yield(uint32_t) {}
void pinMode(uint8_t pin, uint8_t mode) { assert(pin == 13 && mode == INPUT_PULLUP); }
void digitalWrite(uint8_t, uint8_t) { assert(false); } // RX-only meter.
void attachInterruptArg(uint8_t pin, void (*handler)(void *), void *arg, int mode)
{
    assert(pin == 13 && mode == CHANGE);
    rxHandler = handler;
    rxArgument = arg;
}
void detachInterrupt(uint8_t pin) { assert(pin == 13); rxHandler = nullptr; }

// Generate the chip's physical 4800-baud 8E1 signal. The real library's
// GPIO ISR, edge queue and decoder handle it; no decoded bytes are mocked.
static void bit(bool level)
{
    if (bool(gpioInput & (1UL << 13)) != level)
    {
        gpioInput ^= 1UL << 13;
        assert(rxHandler);
        rxHandler(rxArgument);
    }
    ++wireBits;
}

static void send(uint8_t byte)
{
    bit(false);
    for (unsigned i = 0; i < 8; ++i) bit((byte >> i) & 1);
    bit(SoftwareSerial::parityEven(byte));
    bit(true);
}

using Frame = std::array<uint8_t, 24>;
static void checksum(Frame &frame)
{
    frame[23] = 0;
    for (size_t i = 2; i < 23; ++i) frame[23] += frame[i];
}
static void value(Frame &frame, unsigned at, uint32_t v)
{
    frame[at] = v >> 16;
    frame[at + 1] = v >> 8;
    frame[at + 2] = v;
}
static Frame frame(uint32_t powerCycle = 50000)
{
    Frame f = {};
    f[0] = 0x55;
    f[1] = 0x5A;
    value(f, 2, 230000);
    value(f, 5, 1000);
    value(f, 8, 10000);
    value(f, 11, 20000);
    value(f, 14, 5000000);
    value(f, 17, powerCycle);
    f[20] = 0x70;
    f[22] = 1;
    checksum(f);
    return f;
}
static void transmit(const Frame &f)
{
    for (uint8_t byte : f) send(byte);
    wireBits += 12; // Idle after the last stop bit.
}

class Meter : public CSE7766
{
public:
    Meter() : CSE7766(13) {}
    ~Meter() { delete serial; }
    bool overflow() { return serial->overflow(); }
};

int main()
{
    // Negative control: the pre-fix configuration loses the even-parity
    // 0x55/0x5A header bytes because their parity bit is LOW, not a stop bit.
    {
        SoftwareSerial wrong;
        wrong.begin(4800, SWSERIAL_8N1, 13, -1, false, 32);
        transmit(frame());
        std::vector<uint8_t> bytes;
        while (wrong.available()) bytes.push_back(wrong.read());
        assert(bytes.size() < 24);
        for (uint8_t byte : bytes) assert(byte != 0x55 && byte != 0x5A);
        assert(!wrong.overflow());
        printf("REPRO: 8N1 decoded %zu/24 bytes from an 8E1 frame; headers lost, no overflow\n", bytes.size());
    }
    Meter meter;
    transmit(frame(50001));
    meter.loop();
    transmit(frame());
    meter.loop();
    assert(!meter.overflow());
    assert(meter.Energy.data_valid == 0);
    assert(std::fabs(meter.Energy.voltage - 230) < 0.01f);
    assert(std::fabs(meter.Energy.current - 0.5f) < 0.001f);
    assert(std::fabs(meter.Energy.active_power - 100) < 0.01f);
    puts("PASS: real SoftwareSerial ISR/decoder + CSE7766: 230 V, 0.500 A, 100 W");

    // A bad checksum must neither change measurements nor refresh the watchdog.
    Frame changed = frame(25000);
    value(changed, 5, 2000);
    value(changed, 11, 10000);
    checksum(changed);
    Frame corrupt = changed;
    corrupt[23] ^= 1;
    meter.Energy.data_valid = 2;
    transmit(corrupt);
    meter.loop();
    assert(meter.Energy.data_valid == 2);
    assert(std::fabs(meter.Energy.voltage - 230) < 0.01f);

    // Drop a partial packet, then synchronize on the next complete frame.
    for (unsigned i = 0; i < 9; ++i) send(changed[i]);
    meter.loop();
    transmit(changed);
    meter.loop();
    assert(meter.Energy.data_valid == 0);
    assert(std::fabs(meter.Energy.voltage - 115) < 0.01f);
    assert(std::fabs(meter.Energy.current - 1) < 0.001f);
    assert(std::fabs(meter.Energy.active_power - 200) < 0.01f);
    assert(!meter.overflow());

    meter.Energy.daily = 1.25f;
    meter.Energy.total = 65.695f;
    for (unsigned i = 0; i <= ENERGY_WATCHDOG; ++i) meter.everySecond();
    assert(meter.Energy.data_valid > ENERGY_WATCHDOG);
    assert(meter.Energy.voltage == 0 && meter.Energy.current == 0
        && meter.Energy.active_power == 0);
    assert(std::isnan(meter.Energy.apparent_power)
        && std::isnan(meter.Energy.reactive_power) && std::isnan(meter.Energy.power_factor));
    transmit(frame());
    meter.loop();
    assert(meter.Energy.data_valid == 0);
    assert(std::fabs(meter.Energy.voltage - 230) < 0.01f);
    assert(std::fabs(meter.Energy.current - 0.5f) < 0.001f);
    assert(std::fabs(meter.Energy.active_power - 100) < 0.01f);
    assert(meter.Energy.daily == 1.25f && meter.Energy.total == 65.695f);

    // CF pulse accumulation still works after reception recovers.
    meter.everySecond();
    Frame next = frame();
    next[22] = 2;
    checksum(next);
    transmit(next);
    meter.loop();
    assert(meter.everySecond());
    assert(meter.Energy.kWhtoday_delta == 5000 / 36);
    puts("PASS: checksum rejection, truncated-frame resync, watchdog timeout/recovery and CF pulses; totals preserved");
}
