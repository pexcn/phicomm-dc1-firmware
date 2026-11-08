#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

const int U_FLASH = 0;
// Model the core's activation gate; the real core performs MD5 and flash checks.
struct UpdaterClass
{
    std::vector<uint8_t> expected, bytes;
    std::string checksum, supplied;
    size_t capacity = 0;
    bool running = false, activated = false, error = false;
    bool begin(size_t size, int)
    {
        capacity = size;
        bytes.clear();
        supplied.clear();
        running = size != 0;
        activated = error = false;
        return running;
    }
    size_t size() const { return capacity; }
    bool isRunning() const { return running; }
    size_t write(uint8_t *data, size_t size)
    {
        if (!running || error || bytes.size() + size > capacity) { error = true; return 0; }
        bytes.insert(bytes.end(), data, data + size);
        return size;
    }
    bool setMD5(const char *value) { supplied = value; return supplied.size() == 32; }
    bool end(bool partial = false)
    {
        if (running && !error && (partial || bytes.size() == capacity))
        {
            if (bytes == expected && supplied == checksum) activated = true;
            else error = true;
        }
        running = false;
        return activated;
    }
};
extern UpdaterClass Update;
