#pragma once
#include <stdint.h>
#define INPUT 0
#define OUTPUT 1
#define FALLING 2
#define IRAM_ATTR
void delayMicroseconds(unsigned int);
void attachInterrupt(uint8_t, void (*)(), int);
