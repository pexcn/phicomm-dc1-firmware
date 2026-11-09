#pragma once
#include "Config.h"
class Debug
{
public:
    template <typename... Args> static void AddInfo(Args...) {}
    template <typename... Args> static void AddError(Args...) {}
};
class Module
{
public:
    bool failFlush = false;
    bool failSave = false;
    unsigned flushes = 0;
    unsigned saves = 0;
    bool flushRuntime() { ++flushes; return !failFlush; }
    bool saveConfig() { ++saves; return !failSave; }
    void readConfig() {}
    void resetConfig() {}
};
extern Module *module;
