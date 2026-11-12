#include "CAT9554.h"
#include "Wire.h"
#include <cstring>
#include <new>
#include <cstdio>

TestWire Wire;
static unsigned interrupts;
void delayMicroseconds(unsigned int) {}
void attachInterrupt(uint8_t pin, void (*)(), int mode)
{
    assert(pin == 4 && mode == FALLING); ++interrupts;
}

class Probe : public CAT9554 {
public:
    using CAT9554::CAT9554;
    void checkInitialState() const
    {
        assert(outputMask == 0);
        assert(configMask == 0xff && inputMask == 0xff);
        assert(!enableIrq && irqPin == 0);
    }
};

static void configureRelays(CAT9554 &gpio)
{
    // Same input/output pin order as DC1::init().
    for (uint8_t pin : {0, 1, 2}) gpio.pinMode(pin, INPUT);
    for (uint8_t pin : {7, 6, 5, 4}) gpio.pinMode(pin, OUTPUT);
    assert(!Wire.unexpectedHigh);
    assert(Wire.config == 0x0f && Wire.output == 0);
    assert(Wire.readonlyWrites == 0);
}

int main()
{
    for (uint8_t poison : {0xa5, 0xff, 0x00})
    {
        Wire = TestWire{};
        alignas(Probe) uint8_t storage[sizeof(Probe)];
        memset(storage, poison, sizeof(storage));
        auto *gpio = new (storage) Probe(3, 12);
        gpio->checkInitialState();
        gpio->setIrqPin(4);
        gpio->setup();
        configureRelays(*gpio);
        // Later ON commands affect only the requested relay pins.
        Wire.allowHigh = true;
        assert(gpio->digitalWrite(7, true) && Wire.output == 0x80);
        assert(gpio->digitalWrite(6, false) && Wire.output == 0x80);
        assert(gpio->digitalWrite(5, true) && Wire.output == 0xa0);
        assert(gpio->digitalWrite(4, false) && Wire.output == 0xa0);
        assert(gpio->digitalWrite(7, false) && Wire.output == 0x20);
        gpio->~Probe();
    }
    assert(interrupts == 3);

    // ESP-only reboot: the expander retains its old directions/output latch.
    Wire = TestWire{}; Wire.config = 0x0f; Wire.output = 0xf0;
    Probe warm(3, 12); warm.setup(); configureRelays(warm);
    assert(interrupts == 3); // No IRQ requested on this instance.

    // Failed setup reads leave deterministic input-mode, unpressed-button caches.
    Wire = TestWire{}; Wire.inputReadFailures = 20;
    Probe unreadable(3, 12); unreadable.setup();
    assert(unreadable.digitalRead(0));
    configureRelays(unreadable);
    Wire = TestWire{}; Wire.configReadFailures = I2C_RETRY_COUNTER;
    Probe noConfig(3, 12); noConfig.setup(); configureRelays(noConfig);

    // A failed LOW preload must never be followed by an output-enable write.
    Wire = TestWire{}; Wire.outputFailures = I2C_RETRY_COUNTER;
    Probe failed(3, 12); failed.setup(); failed.pinMode(7, OUTPUT);
    assert(Wire.config == 0xff && Wire.configAttempts == 0 && !Wire.unexpectedHigh);
    failed.pinMode(6, OUTPUT);
    assert(Wire.config == 0x3f && Wire.output == 0 && !Wire.unexpectedHigh);

    // Successful final retry still permits the direction write.
    Wire = TestWire{}; Wire.outputFailures = I2C_RETRY_COUNTER - 1;
    Probe lastRetry(3, 12); lastRetry.setup(); lastRetry.pinMode(7, OUTPUT);
    assert(Wire.outputAttempts == I2C_RETRY_COUNTER && Wire.config == 0x7f);
    assert(!Wire.unexpectedHigh);

    Wire = TestWire{}; Wire.configFailures = I2C_RETRY_COUNTER;
    Probe directionFailure(3, 12); directionFailure.setup(); directionFailure.pinMode(7, OUTPUT);
    assert(Wire.config == 0xff && Wire.output == 0 && !Wire.unexpectedHigh);
    puts("PASS: actual CAT9554: poisoned allocation, latch-before-direction, warm boot, read/write failures, final retry and isolated relay writes");
}
