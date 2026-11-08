#include <array>
#include <cassert>
#include <cstdio>
#include "Module.h"
#include "spi_flash.h"
#include "EnergyState.h"
#include "RuntimeStorage.h"
#include "LittleFS.h"
#include "DC1Config.pb.h"

static std::array<uint8_t, 1024 * 1024> flash;
static std::array<unsigned, 256> erases = {};
static unsigned long now;
static bool failErase, failWrite, corruptWrite;
static const unsigned CONFIG = 0xfb000;
unsigned long millis() { return now; }

int spi_flash_read(uint32 address, uint32 *data, uint32 size)
{
    assert(address % 4 == 0 && size % 4 == 0 && uintptr_t(data) % 4 == 0);
    assert(address + size <= flash.size());
    memcpy(data, flash.data() + address, size);
    return 0;
}
int spi_flash_erase_sector(uint16 sector)
{
    assert(sector == CONFIG / 4096);
    if (failErase) { failErase = false; return 1; }
    ++erases[sector];
    memset(flash.data() + sector * 4096, 0xff, 4096);
    return 0;
}
int spi_flash_write(uint32 address, uint32 *data, uint32 size)
{
    assert(address % 4 == 0 && size % 4 == 0 && uintptr_t(data) % 4 == 0);
    assert(address == CONFIG && size == 4096);
    const uint8_t *source = reinterpret_cast<uint8_t *>(data);
    uint32 limit = failWrite ? 8 : size;
    for (uint32 i = 0; i < limit; ++i) flash[address + i] &= source[i];
    if (failWrite) { failWrite = false; return 1; }
    if (corruptWrite) { corruptWrite = false; flash[address] ^= 1; }
    return 0;
}

static void testConfiguration()
{
    flash.fill(0xff);
    for (now = 0; now <= 86400000; now += 1000) Config::loop();
    assert(erases[CONFIG / 4096] == 0); // Idle for a day: no writes at all.
    Config::resetConfig();
    strcpy(globalConfig.wifi.ssid, "example");
    assert(Config::saveConfig());
    assert(erases[CONFIG / 4096] == 1);
    assert(Config::saveConfig());
    assert(erases[CONFIG / 4096] == 1); // Identical settings submissions.
    globalConfig.mqtt.port = 1883;
    assert(Config::saveConfig());
    assert(erases[CONFIG / 4096] == 2);
    memset(&globalConfig, 0, sizeof(globalConfig));
    Config::readConfig();
    assert(strcmp(globalConfig.wifi.ssid, "example") == 0 && globalConfig.mqtt.port == 1883);

    // Each kind of failure must leave a retry pending, even for the same data.
    for (int kind = 0; kind < 3; ++kind)
    {
        ++globalConfig.mqtt.port;
        failErase = kind == 0;
        failWrite = kind == 1;
        corruptWrite = kind == 2;
        assert(!Config::saveConfig());
        unsigned attempts = erases[CONFIG / 4096];
        now += 59999;
        Config::loop();
        assert(erases[CONFIG / 4096] == attempts);
        ++now;
        Config::loop();
        assert(erases[CONFIG / 4096] == attempts + 1);
        uint16_t expected = globalConfig.mqtt.port;
        Config::readConfig();
        assert(globalConfig.mqtt.port == expected);
    }
    unsigned committed = erases[CONFIG / 4096];
    now += 100000;
    Config::loop();
    assert(erases[CONFIG / 4096] == committed);
}

static void testRestartPreparation()
{
    Module current;
    module = &current;
    current.failFlush = true;
    ++globalConfig.mqtt.port;
    unsigned before = erases[CONFIG / 4096];
    assert(!Config::prepareRestart());
    assert(current.flushes == 1 && erases[CONFIG / 4096] == before);
    current.failFlush = false;
    assert(Config::prepareRestart());
    assert(current.flushes == 2 && erases[CONFIG / 4096] == before + 1);
    module = nullptr;
}

static void testRetiredConfigFields()
{
    // A valid older configuration can exceed the new schema's encoded maximum.
    Config::resetConfig();
    auto fill = [](char *value, size_t size) { memset(value, 'x', size - 1); };
    fill(globalConfig.wifi.ssid, sizeof(globalConfig.wifi.ssid));
    fill(globalConfig.wifi.pass, sizeof(globalConfig.wifi.pass));
    fill(globalConfig.wifi.ip, sizeof(globalConfig.wifi.ip));
    fill(globalConfig.wifi.sn, sizeof(globalConfig.wifi.sn));
    fill(globalConfig.wifi.gw, sizeof(globalConfig.wifi.gw));
    fill(globalConfig.wifi.ntp, sizeof(globalConfig.wifi.ntp));
    fill(globalConfig.mqtt.server, sizeof(globalConfig.mqtt.server));
    fill(globalConfig.mqtt.user, sizeof(globalConfig.mqtt.user));
    fill(globalConfig.mqtt.pass, sizeof(globalConfig.mqtt.pass));
    fill(globalConfig.mqtt.topic, sizeof(globalConfig.mqtt.topic));
    fill(globalConfig.mqtt.discovery_prefix, sizeof(globalConfig.mqtt.discovery_prefix));
    fill(globalConfig.uid, sizeof(globalConfig.uid));
    globalConfig.module_cfg.size = sizeof(globalConfig.module_cfg.bytes);
    memset(globalConfig.module_cfg.bytes, 0x5a, globalConfig.module_cfg.size);
    auto expected = globalConfig;

    uint8_t oldHttp[200], retired[149];
    memset(retired, 'u', sizeof(retired));
    HttpConfigMessage http = {};
    http.port = 8080;
    strcpy(http.user, "admin");
    strcpy(http.pass, "secret");
    auto submessage = pb_ostream_from_buffer(oldHttp, sizeof(oldHttp));
    assert(pb_encode(&submessage, HttpConfigMessage_fields, &http));
    assert(pb_encode_tag(&submessage, PB_WT_STRING, 4));
    assert(pb_encode_string(&submessage, retired, sizeof(retired)));
    uint8_t oldDebug[64];
    DebugConfigMessage debug = {};
    debug.type = 4;
    auto logging = pb_ostream_from_buffer(oldDebug, sizeof(oldDebug));
    assert(pb_encode(&logging, DebugConfigMessage_fields, &debug));
    assert(pb_encode_tag(&logging, PB_WT_STRING, 2));
    assert(pb_encode_string(&logging, retired, 39));
    assert(pb_encode_tag(&logging, PB_WT_VARINT, 3));
    assert(pb_encode_varint(&logging, 514));
    uint8_t payload[1200];
    auto stream = pb_ostream_from_buffer(payload, sizeof(payload));
    assert(pb_encode(&stream, GlobalConfigMessage_fields, &globalConfig));
    assert(pb_encode_tag(&stream, PB_WT_STRING, 2));
    assert(pb_encode_string(&stream, oldHttp, submessage.bytes_written));
    assert(pb_encode_tag(&stream, PB_WT_STRING, 4));
    assert(pb_encode_string(&stream, oldDebug, logging.bytes_written));
    size_t size = stream.bytes_written;
    assert(size > GlobalConfigMessage_size && size <= 1099); // Previous schema limit.
    uint16_t crc = Config::crc16(payload, size);
    uint8_t header[] = {uint8_t(GLOBAL_CFG_VERSION >> 8), uint8_t(GLOBAL_CFG_VERSION),
        uint8_t(size >> 8), uint8_t(size), uint8_t(crc >> 8), uint8_t(crc)};
    memcpy(flash.data() + CONFIG, header, sizeof(header));
    memcpy(flash.data() + CONFIG + sizeof(header), payload, size);
    memset(&globalConfig, 0, sizeof(globalConfig));
    Config::readConfig();
    assert(strcmp(globalConfig.wifi.ssid, expected.wifi.ssid) == 0);
    assert(strcmp(globalConfig.mqtt.pass, expected.mqtt.pass) == 0);
    assert(globalConfig.http.port == http.port && strcmp(globalConfig.http.user, http.user) == 0);
    assert(strcmp(globalConfig.http.pass, http.pass) == 0);
    assert(globalConfig.debug.type == debug.type);
    assert(Config::saveConfig());
    assert((unsigned(flash[CONFIG + 2]) << 8 | flash[CONFIG + 3]) <= GlobalConfigMessage_size);
}

static void testRuntimeStorage()
{
    assert(!RuntimeStorage::begin());
    assert(!RuntimeStorage::ready());
    assert(!LittleFS.autoFormat && LittleFS.formatCalls == 0);
    assert(!RuntimeStorage::saveRelay(5));
    assert(RuntimeStorage::initialize());
    assert(LittleFS.formatCalls == 1);
    assert(RuntimeStorage::initialize() && LittleFS.formatCalls == 1);
    auto configuration = flash;
    assert(RuntimeStorage::saveRelay(5));
    uint8_t state = 0;
    assert(RuntimeStorage::loadRelay(state) == RuntimeStorage::Result::Ok && state == 5);
    unsigned mutations = LittleFS.mutations;
    assert(RuntimeStorage::saveRelay(5));
    assert(LittleFS.mutations == mutations);
    EnergyState energy;
    energy.add(UINT32_MAX);
    energy.add(123);
    energy.updateDay(20000);
    energy.startTime = 1728000000;
    assert(RuntimeStorage::saveEnergy(energy));
    EnergyState restored;
    assert(RuntimeStorage::loadEnergy(restored) == RuntimeStorage::Result::Ok);
    assert(restored.total == energy.total && restored.day == energy.day
        && restored.startTime == energy.startTime);
    EnergyState invalid = energy;
    invalid.today = invalid.total + 1;
    assert(!RuntimeStorage::saveEnergy(invalid));
    mutations = LittleFS.mutations;
    assert(RuntimeStorage::saveEnergy(energy) && LittleFS.mutations == mutations);
    LittleFS.shortWrite = true;
    assert(!RuntimeStorage::saveRelay(3));
    assert(RuntimeStorage::loadRelay(state) == RuntimeStorage::Result::Ok && state == 5);
    LittleFS.corruptWrite = true;
    assert(!RuntimeStorage::saveRelay(3));
    assert(RuntimeStorage::loadRelay(state) == RuntimeStorage::Result::Ok && state == 5);
    LittleFS.failRename = true;
    assert(!RuntimeStorage::saveRelay(3));
    assert(RuntimeStorage::loadRelay(state) == RuntimeStorage::Result::Ok && state == 5);
    LittleFS.failRename = false;
    assert(RuntimeStorage::saveRelay(3));
    assert(memcmp(flash.data() + CONFIG, configuration.data() + CONFIG, 4096) == 0);

    // Interrupt every mutation, including metadata compaction and block rotation.
    for (int compact = 0; compact < 2; ++compact)
    for (int type = 0; type < 2; ++type)
    {
        assert(RuntimeStorage::saveRelay(3));
        assert(RuntimeStorage::saveEnergy(energy));
        std::array<uint8_t, 65536> baseline;
        EnergyState previous, updated;
        uint8_t previousRelay = 0, nextRelay = 0;
        unsigned steps = 0;
        for (unsigned prepare = 0; ; ++prepare)
        {
            assert(prepare < 1024);
            RuntimeStorage::end();
            baseline = LittleFS.flash;
            assert(RuntimeStorage::begin());
            assert(RuntimeStorage::loadRelay(previousRelay) == RuntimeStorage::Result::Ok);
            assert(RuntimeStorage::loadEnergy(previous) == RuntimeStorage::Result::Ok);
            nextRelay = previousRelay ^ 4;
            updated = previous;
            updated.add(456);
            unsigned before = LittleFS.mutations, beforeErases = 0;
            for (unsigned count : LittleFS.erases) beforeErases += count;
            assert(type == 0 ? RuntimeStorage::saveRelay(nextRelay) : RuntimeStorage::saveEnergy(updated));
            steps = LittleFS.mutations - before;
            unsigned afterErases = 0;
            for (unsigned count : LittleFS.erases) afterErases += count;
            if (!compact || afterErases > beforeErases) break;
        }
        for (unsigned cut = 0; cut < steps; ++cut)
        {
            RuntimeStorage::end();
            LittleFS.flash = baseline;
            assert(RuntimeStorage::begin());
            uint8_t scratch;
            EnergyState scratchEnergy;
            assert(RuntimeStorage::loadRelay(scratch) == RuntimeStorage::Result::Ok);
            assert(RuntimeStorage::loadEnergy(scratchEnergy) == RuntimeStorage::Result::Ok);
            LittleFS.failAfter = cut;
            bool result = type == 0 ? RuntimeStorage::saveRelay(nextRelay) : RuntimeStorage::saveEnergy(updated);
            assert(!result);
            RuntimeStorage::end();
            LittleFS.powerOff = false;
            LittleFS.failAfter = -1;
            assert(RuntimeStorage::begin());
            assert(RuntimeStorage::loadRelay(state) == RuntimeStorage::Result::Ok);
            assert(state == previousRelay || (type == 0 && state == nextRelay));
            assert(RuntimeStorage::loadEnergy(restored) == RuntimeStorage::Result::Ok);
            assert(restored.total == previous.total || (type == 1 && restored.total == updated.total));
            assert(restored.day == previous.day && LittleFS.formatCalls == 1);
        }
        printf("PASS: %u LittleFS %s power-cut points (%s)\n", steps,
            type == 0 ? "relay" : "energy", compact ? "compaction" : "normal update");
    }
    // Failed mount of existing storage never triggers formatting or alters bytes.
    RuntimeStorage::end();
    auto saved = LittleFS.flash;
    LittleFS.failMount = true;
    assert(!RuntimeStorage::begin());
    assert(!RuntimeStorage::saveRelay(4));
    assert(LittleFS.flash == saved && LittleFS.formatCalls == 1);
    LittleFS.failMount = false;
    RuntimeStorage::end();
    assert(RuntimeStorage::begin());
    assert(memcmp(flash.data() + CONFIG, configuration.data() + CONFIG, 4096) == 0);
}

static void testCorruptRecordRecovery()
{
    const uint8_t invalid[] = {1, 2, 3};
    EnergyState before, recovered;
    uint8_t relayBefore, relayAfter;
    assert(RuntimeStorage::loadEnergy(before) == RuntimeStorage::Result::Ok);
    assert(RuntimeStorage::loadRelay(relayBefore) == RuntimeStorage::Result::Ok);
    auto file = LittleFS.open("/energy.bin", "w");
    assert(file && file.write(invalid, sizeof(invalid)) == sizeof(invalid));
    file.close();
    auto damaged = LittleFS.flash;
    assert(RuntimeStorage::loadEnergy(recovered) == RuntimeStorage::Result::Invalid);
    assert(!RuntimeStorage::saveEnergy(before) && LittleFS.flash == damaged);
    assert(RuntimeStorage::saveEnergy(before, true));
    assert(RuntimeStorage::loadRelay(relayAfter) == RuntimeStorage::Result::Ok && relayAfter == relayBefore);
    file = LittleFS.open("/relay.bin", "w");
    assert(file && file.write(invalid, sizeof(invalid)) == sizeof(invalid));
    file.close();
    damaged = LittleFS.flash;
    assert(RuntimeStorage::loadRelay(relayAfter) == RuntimeStorage::Result::Invalid);
    assert(!RuntimeStorage::saveRelay(3) && LittleFS.flash == damaged);
    assert(RuntimeStorage::saveRelay(3, true));
    assert(RuntimeStorage::loadRelay(relayAfter) == RuntimeStorage::Result::Ok && relayAfter == 3);
    assert(RuntimeStorage::loadEnergy(recovered) == RuntimeStorage::Result::Ok && recovered.total == before.total);
}

static void testLegacyDecoding()
{
    // Old tag 1: relay mask; tags 25..29: runtime energy. New settings skip them.
    const uint8_t legacy[] = {0x08, 0x0b, 0x10, 0x03, 0xc8, 0x01, 0x7b, 0xd0, 0x01, 0x32,
                              0xd8, 0x01, 0x7b, 0xe0, 0x01, 0x03, 0xe8, 0x01, 0x64};
    DC1ConfigMessage config = {};
    pb_istream_t input = pb_istream_from_buffer(legacy, sizeof(legacy));
    assert(pb_decode(&input, DC1ConfigMessage_fields, &config));
    assert(config.power_on_state == 3);
    DC1LegacyStateMessage state = {};
    input = pb_istream_from_buffer(legacy, sizeof(legacy));
    assert(pb_decode(&input, DC1LegacyStateMessage_fields, &state) && state.last_state == 11);
    assert(state.energy_kWhtoday == 123 && state.energy_kWhyesterday == 50
        && state.energy_kWhtotal == 123 && state.energy_kWhdoy == 3
        && state.energy_kWhtotal_time == 100);
    uint8_t output[DC1ConfigMessage_size];
    pb_ostream_t stream = pb_ostream_from_buffer(output, sizeof(output));
    assert(pb_encode(&stream, DC1ConfigMessage_fields, &config));
    assert(stream.bytes_written == 2 && output[0] == 0x10 && output[1] == 3);
}

int main()
{
    testConfiguration();
    testRestartPreparation();
    testRetiredConfigFields();
    testRuntimeStorage();
    testCorruptRecordRecovery();
    testLegacyDecoding();
    puts("PASS: energy snapshots, event-driven settings, retries, isolation, legacy decoding");
}
