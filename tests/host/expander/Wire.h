#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <cassert>

struct TestWire {
    // CAT9554 reset values: all inputs, output latch HIGH, no inversion.
    uint8_t config = 0xff, output = 0xff, inputs = 0xff, selected = 0;
    unsigned outputFailures = 0, configFailures = 0, inputReadFailures = 0, configReadFailures = 0;
    unsigned outputAttempts = 0, configAttempts = 0, readonlyWrites = 0;
    bool unexpectedHigh = false, allowHigh = false, ready = false;
    std::vector<uint8_t> tx;
    void begin(uint8_t, uint8_t) {}
    void setClock(uint16_t) {}
    void beginTransmission(uint8_t address) { assert(address == 0x20); tx.clear(); }
    size_t write(uint8_t value) { tx.push_back(value); return 1; }
    int endTransmission(bool)
    {
        assert(tx.size() == 1 || tx.size() == 2);
        selected = tx[0];
        if (tx.size() == 1)
        {
            unsigned *failures = selected == 0 ? &inputReadFailures : &configReadFailures;
            if (*failures) { --*failures; return 2; }
            return 0;
        }
        if (selected == 1)
        {
            ++outputAttempts;
            if (outputFailures) { --outputFailures; return 2; }
            output = tx[1];
        }
        else if (selected == 3)
        {
            ++configAttempts;
            if (configFailures) { --configFailures; return 2; }
            config = tx[1];
        }
        else if (selected == 0) ++readonlyWrites; // Hardware ignores input-register writes.
        else assert(false);
        if (!allowHigh && (output & ~config & 0xf0)) unexpectedHigh = true;
        return 0;
    }
    void requestFrom(int address, int count) { assert(address == 0x20 && count == 1); ready = true; }
    int available() { return ready ? 1 : 0; }
    int read()
    {
        assert(ready); ready = false;
        return selected == 3 ? config : (inputs & config) | (output & ~config);
    }
};
extern TestWire Wire;
