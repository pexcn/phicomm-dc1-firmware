#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
using std::min;
using std::isnan;

#define IRAM_ATTR
#define INPUT 0
#define INPUT_PULLUP 2
#define OUTPUT 1
#define OUTPUT_OPEN_DRAIN 3
#define LOW 0
#define HIGH 1
#define CHANGE 1
#define RISING 2
#define FALLING 3

extern volatile uint32_t gpioInput;
uint32_t micros();
unsigned long millis();
void delay(unsigned long ms);
void optimistic_yield(uint32_t);
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t level);
void attachInterruptArg(uint8_t pin, void (*handler)(void *), void *arg, int mode);
void detachInterrupt(uint8_t pin);
inline uint8_t digitalPinToPort(uint8_t pin) { return pin; }
inline uint32_t digitalPinToBitMask(uint8_t pin) { return 1UL << pin; }
inline uint8_t digitalPinToInterrupt(uint8_t pin) { return pin; }
inline volatile uint32_t *portInputRegister(uint8_t) { return &gpioInput; }
inline volatile uint32_t *portOutputRegister(uint8_t) { return &gpioInput; }
inline uint32_t xt_rsil(uint32_t) { return 0; }
inline void xt_wsr_ps(uint32_t) {}
inline uint8_t pgm_read_byte(const uint8_t *ptr) { return *ptr; }
