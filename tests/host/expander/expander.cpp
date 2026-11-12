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
    uint16_t cachedOutput() const { return outputMask; }
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

static void testFailedOutputWrites()
{
    for (bool initiallyOn : {false, true})
    {
        const uint8_t pin = 6, other = 5;
        Wire = TestWire{};
        Probe gpio(3, 12); gpio.setup(); configureRelays(gpio);
        Wire.allowHigh = true;
        if (initiallyOn) assert(gpio.digitalWrite(pin, true));
        const uint8_t before = Wire.output;
        const unsigned attempts = Wire.outputAttempts;
        // Match switchRelay's two calls, each exhausting its I2C retries.
        Wire.outputFailures = 2 * I2C_RETRY_COUNTER;
        for (unsigned call = 0; call < 2; ++call)
        {
            assert(!gpio.digitalWrite(pin, !initiallyOn));
            assert(Wire.output == before && gpio.cachedOutput() == before);
        }
        assert(Wire.outputAttempts == attempts + 2 * I2C_RETRY_COUNTER);
        // Neither another output nor a configuration write may replay the failed change.
        const unsigned nextAt = Wire.outputAttempts;
        assert(gpio.digitalWrite(other, true));
        assert(Wire.outputAttempts == nextAt + 1);
        const uint8_t expected = before | (1U << other);
        assert(Wire.output == expected && gpio.cachedOutput() == expected);
        gpio.pinMode(0, INPUT);
        assert(Wire.output == expected && gpio.cachedOutput() == expected);

        // A successful final retry must commit the new cache and output together.
        Wire.outputFailures = I2C_RETRY_COUNTER - 1;
        const unsigned retryAt = Wire.outputAttempts;
        assert(gpio.digitalWrite(pin, !initiallyOn));
        assert(Wire.outputAttempts == retryAt + I2C_RETRY_COUNTER);
        assert(Wire.output == (expected ^ (1U << pin)) && gpio.cachedOutput() == Wire.output);
    }
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
    testFailedOutputWrites();
    puts("PASS: actual CAT9554: initialized safe outputs, warm boot, I2C failures and failed-write cache isolation");
}
