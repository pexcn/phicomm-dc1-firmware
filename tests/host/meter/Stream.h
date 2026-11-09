#pragma once
#include "Arduino.h"
#include <cstddef>

class Print
{
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *, size_t) = 0;
};

class Stream : public Print
{
protected:
    unsigned long _timeout = 1000;
public:
    virtual int available() = 0;
    virtual int availableForWrite() = 0;
    virtual int peek() = 0;
    virtual int read() = 0;
    virtual int read(uint8_t *, size_t) = 0;
    virtual size_t readBytes(uint8_t *, size_t) = 0;
    virtual size_t readBytes(char *, size_t) = 0;
    virtual void flush() = 0;
};
