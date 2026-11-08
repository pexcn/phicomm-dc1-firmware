#ifndef RUNTIME_STORAGE_H
#define RUNTIME_STORAGE_H

#include "EnergyState.h"

// LittleFS owns only the runtime partition, never the configuration sector.
class RuntimeStorage
{
public:
    enum class Result { Ok, Missing, Invalid, Unavailable };
    static bool begin();
    static void end();
    static bool ready();
    // Explicit user action only. A mounted filesystem is never reformatted.
    static bool initialize();
    static Result loadRelay(uint8_t &state);
    static bool readLegacyRelay(uint8_t &state);
    static bool saveRelay(uint8_t state, bool recover = false);
    static Result loadEnergy(EnergyState &energy);
    static bool saveEnergy(const EnergyState &energy, bool recover = false);
};

#endif
