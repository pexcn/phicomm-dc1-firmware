#ifndef _CONFIG_h
#define _CONFIG_h
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#define PSTR(text) text
#define F(text) text
#define snprintf_P snprintf
#define vsnprintf_P vsnprintf
#define memcpy_P memcpy
#define WEB_LOG_SIZE 4000
using PGM_P = const char *;
struct LogConfig { struct { uint8_t type = 0; } debug; };
extern LogConfig globalConfig;
extern char tmpData[512];
extern uint32_t perSecond;
struct SerialSink {
    char data[1024] = {};
    size_t used = 0;
    size_t write(const uint8_t *bytes, size_t size)
    {
        size_t available = sizeof(data) - used - 1;
        size_t copied = size < available ? size : available;
        memcpy(data + used, bytes, copied);
        used += copied;
        data[used] = 0;
        return size;
    }
};
extern SerialSink Serial, Serial1;
#endif
