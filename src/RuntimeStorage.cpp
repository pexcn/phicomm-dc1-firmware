#include <LittleFS.h>
#include <c_types.h>
#include <spi_flash.h>
#include <string.h>
#include "RuntimeStorage.h"

namespace {
bool attempted = false;
bool mounted = false;
const char *RELAY_PATH = "/relay.bin";
const char *ENERGY_PATH = "/energy.bin";
const char *TEMP_PATH = "/runtime.tmp";
const size_t RELAY_SIZE = 12;
const size_t ENERGY_SIZE = 44;
const uint32_t RELAY_MAGIC = 0x44435201;
const uint32_t ENERGY_MAGIC = 0x44434501;

void put(uint8_t *data, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i) { data[i] = value & 0xff; value >>= 8; }
}
uint64_t get(const uint8_t *data, size_t size)
{
    uint64_t value = 0;
    for (size_t i = size; i > 0; --i) value = (value << 8) | data[i - 1];
    return value;
}
uint32_t checksum(const uint8_t *data, size_t size)
{
    uint32_t crc = ~0U;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
RuntimeStorage::Result read(const char *path, uint8_t *data, size_t size, uint32_t magic)
{
    if (!mounted) return RuntimeStorage::Result::Unavailable;
    if (!LittleFS.exists(path)) return RuntimeStorage::Result::Missing;
    auto file = LittleFS.open(path, "r");
    if (!file || file.size() != size || file.read(data, size) != int(size))
        return RuntimeStorage::Result::Invalid;
    file.close();
    if (get(data, 4) != magic || get(data + size - 4, 4) != checksum(data, size - 4))
        return RuntimeStorage::Result::Invalid;
    if (magic == RELAY_MAGIC && get(data + 4, 4) > 15)
        return RuntimeStorage::Result::Invalid;
    if (magic == ENERGY_MAGIC && (get(data + 4, 8) > get(data + 20, 8)
        || get(data + 12, 8) > get(data + 20, 8)
        || (get(data + 36, 4) != UINT32_MAX && get(data + 36, 4) > 365)))
        return RuntimeStorage::Result::Invalid;
    return RuntimeStorage::Result::Ok;
}
bool write(const char *path, uint8_t *data, size_t size, uint32_t magic, bool recover)
{
    if (!mounted) return false;
    put(data, magic, 4);
    put(data + size - 4, checksum(data, size - 4), 4);
    uint8_t previous[ENERGY_SIZE];
    RuntimeStorage::Result result = read(path, previous, size, magic);
    if (result == RuntimeStorage::Result::Invalid && !recover) return false;
    if (result == RuntimeStorage::Result::Ok && memcmp(previous, data, size) == 0) return true;
    auto file = LittleFS.open(TEMP_PATH, "w");
    if (!file) return false;
    size_t written = file.write(data, size);
    file.flush();
    file.close();
    // File::flush/close do not return SDK errors. Reopen and verify the record.
    if (written != size || read(TEMP_PATH, previous, size, magic) != RuntimeStorage::Result::Ok
        || memcmp(previous, data, size) != 0) return false;
    // Never remove the old file first: rename is the atomic commit point.
    if (!LittleFS.rename(TEMP_PATH, path)) return false;
    return read(path, previous, size, magic) == RuntimeStorage::Result::Ok
        && memcmp(previous, data, size) == 0;
}
}

bool RuntimeStorage::begin()
{
    if (!attempted)
    {
        attempted = true;
        LittleFS.setConfig(LittleFSConfig(false));
        mounted = LittleFS.begin();
    }
    return mounted;
}
bool RuntimeStorage::ready() { return mounted; }
void RuntimeStorage::end()
{
    if (mounted) LittleFS.end();
    mounted = false;
    attempted = false;
}
bool RuntimeStorage::initialize()
{
    if (mounted) return true;
    LittleFS.setConfig(LittleFSConfig(false));
    attempted = true;
    mounted = LittleFS.format() && LittleFS.begin();
    return mounted;
}
RuntimeStorage::Result RuntimeStorage::loadRelay(uint8_t &state)
{
    uint8_t data[RELAY_SIZE];
    Result result = read(RELAY_PATH, data, sizeof(data), RELAY_MAGIC);
    if (result != Result::Ok) return result;
    state = get(data + 4, 4);
    return Result::Ok;
}
bool RuntimeStorage::saveRelay(uint8_t state, bool recover)
{
    uint8_t data[RELAY_SIZE] = {};
    put(data + 4, state & 15, 4);
    return write(RELAY_PATH, data, sizeof(data), RELAY_MAGIC, recover);
}
RuntimeStorage::Result RuntimeStorage::loadEnergy(EnergyState &energy)
{
    uint8_t data[ENERGY_SIZE];
    Result result = read(ENERGY_PATH, data, sizeof(data), ENERGY_MAGIC);
    if (result != Result::Ok) return result;
    EnergyState loaded;
    loaded.today = get(data + 4, 8);
    loaded.yesterday = get(data + 12, 8);
    loaded.total = get(data + 20, 8);
    loaded.day = get(data + 28, 4);
    loaded.startTime = get(data + 32, 4);
    loaded.legacyDay = get(data + 36, 4);
    energy = loaded;
    return Result::Ok;
}
bool RuntimeStorage::saveEnergy(const EnergyState &energy, bool recover)
{
    if (energy.today > energy.total || energy.yesterday > energy.total
        || (energy.legacyDay != UINT32_MAX && energy.legacyDay > 365)) return false;
    uint8_t data[ENERGY_SIZE] = {};
    put(data + 4, energy.today, 8);
    put(data + 12, energy.yesterday, 8);
    put(data + 20, energy.total, 8);
    put(data + 28, energy.day, 4);
    put(data + 32, energy.startTime, 4);
    put(data + 36, energy.legacyDay, 4);
    return write(ENERGY_PATH, data, sizeof(data), ENERGY_MAGIC, recover);
}

// Compatibility only: the previous relay sector is never written again.
bool RuntimeStorage::readLegacyRelay(uint8_t &state)
{
    extern uint32_t _RELAY_STATE_start;
    const uint32_t address = (uintptr_t)&_RELAY_STATE_start - 0x40200000;
    const uint32_t magic = 0x44433101;
    uint32_t record[3];
    if (spi_flash_read(address, record, sizeof(record)) != SPI_FLASH_RESULT_OK
        || record[0] != magic || (record[1] & ~0x0fU) != 0
        || record[2] != ~(record[0] ^ record[1])) return false;
    state = record[1];
    return true;
}
