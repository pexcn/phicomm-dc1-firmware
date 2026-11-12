#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using byte = uint8_t;
using boolean = bool;
#define PSTR(text) text
#define pgm_read_byte_near(address) (*(const uint8_t *)(address))
inline unsigned long millis() { return 1; }
inline void yield() {}
class IPAddress {
public:
    IPAddress() = default;
    IPAddress(uint8_t, uint8_t, uint8_t, uint8_t) {}
};
class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *, size_t) = 0;
};
class Stream : public Print {};
class Client : public Stream {
    bool up = false;
    size_t position = 0;
public:
    std::vector<uint8_t> incoming, outgoing;
    int connect(const char *, uint16_t) { up = true; return 1; }
    int connect(IPAddress, uint16_t) { up = true; return 1; }
    bool connected() const { return up; }
    int available() const { return incoming.size() - position; }
    int read() { return incoming.at(position++); }
    void stop() { up = false; }
    void flush() {}
    size_t write(uint8_t value) override { outgoing.push_back(value); return 1; }
    size_t write(const uint8_t *data, size_t size) override
    {
        outgoing.insert(outgoing.end(), data, data + size);
        return size;
    }
};
