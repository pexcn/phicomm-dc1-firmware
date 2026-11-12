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
    size_t write(const uint8_t *, size_t size) { return size; }
};
extern SerialSink Serial, Serial1;
#endif
