#include <array>
#include <cassert>
#include <cstdio>
#include "Module.h"
#include "spi_flash.h"
#include "EnergyState.h"
#include "RuntimeStorage.h"
#include "LittleFS.h"
#include "DC1Config.pb.h"
#include "coredecls.h"

static std::array<uint8_t, 1024 * 1024> flash;
static std::array<unsigned, 256> erases = {};
static unsigned long now;
static bool failErase, failWrite, corruptWrite;
static const unsigned CONFIG = 0xfb000;
static const unsigned BACKUP = 0x74000;
static int cutAfter = -1;
static unsigned mutations = 0;
static bool powerOff = false, failRead = false;
static unsigned portals, stations;
static void delay(unsigned) {}
struct Wifi {
    static void connectWifi();
    static void setupWifi() { ++stations; }
    static void setupWifiManager(bool resetSettings)
    {
        ++portals;
        if (resetSettings) Config::resetConfig();
    }
};
#include "wifi-connect.inc"
static unsigned configErases() { return erases[CONFIG / 4096] + erases[BACKUP / 4096]; }
static bool mutation()
{
    ++mutations;
    if (cutAfter >= 0 && mutations > unsigned(cutAfter)) { powerOff = true; return false; }
    return true;
}
static void assertConfigIsolation(const std::array<uint8_t, 1024 * 1024> &before)
{
    for (unsigned address = 0; address < flash.size(); address += 4096)
        if (address != CONFIG && address != BACKUP)
            assert(memcmp(flash.data() + address, before.data() + address, 4096) == 0);
}
unsigned long millis() { return now; }

int spi_flash_read(uint32 address, uint32 *data, uint32 size)
{
    assert(address % 4 == 0 && size % 4 == 0 && uintptr_t(data) % 4 == 0);
    assert(address + size <= flash.size());
    if (powerOff || failRead) { failRead = false; return 1; }
    memcpy(data, flash.data() + address, size);
    return 0;
}
int spi_flash_erase_sector(uint16 sector)
{
    assert(sector == CONFIG / 4096 || sector == BACKUP / 4096);
    if (powerOff) return 1;
    if (failErase) { failErase = false; return 1; }
    ++erases[sector];
    for (unsigned offset = 0; offset < 4096; offset += 4)
    {
        memset(flash.data() + sector * 4096 + offset, 0xff, 4);
        if (!mutation()) return 1;
    }
    return 0;
}
int spi_flash_write(uint32 address, uint32 *data, uint32 size)
{
    assert(address % 4 == 0 && size % 4 == 0 && uintptr_t(data) % 4 == 0);
    unsigned base = address & ~4095U;
    assert((base == CONFIG || base == BACKUP) && address + size <= base + 4096);
    if (powerOff) return 1;
    const uint8_t *source = reinterpret_cast<uint8_t *>(data);
    uint32 limit = failWrite && size > 8 ? 8 : size;
    for (uint32 offset = 0; offset < limit; offset += 4)
    {
        for (uint32 i = 0; i < 4; ++i) flash[address + offset + i] &= source[offset + i];
        if (!mutation()) return 1;
    }
    if (failWrite) { failWrite = false; return 1; }
    if (corruptWrite) { corruptWrite = false; flash[address] ^= 1; }
    return 0;
}

static void testInitialProvisioning()
{
    flash.fill(0xff);
    Config::resetConfig();
    strcpy(globalConfig.http.pass, "2$000102030405060708090a0b0c0d0e0f$6bb6535cc4adff6d2138573f125b9fea7135129df687ca37139a6720c5c8b8e0");
    strcpy(globalConfig.mqtt.server, "mqtt.lan");
    strcpy(globalConfig.uid, "my-dc1");
    assert(globalConfig.wifi.ssid[0] == '\0' && Config::saveConfig());
    const auto saved = globalConfig;
    const unsigned before = configErases();
    memset(&globalConfig, 0, sizeof(globalConfig));
    Config::readConfig();
    Wifi::connectWifi();
    assert(memcmp(&globalConfig, &saved, sizeof(saved)) == 0);
    assert(Config::saveConfig() && configErases() == before);
    assert(portals == 1 && stations == 0);
    strcpy(globalConfig.wifi.ssid, "home-network");
    Wifi::connectWifi();
    assert(portals == 1 && stations == 1);
    puts("PASS: actual initial Wi-Fi dispatch preserves saved admin credentials/configuration across provisioning reboots without flash writes");
}

static void testConfiguration()
{
    flash.fill(0xff);
    for (now = 0; now <= 86400000; now += 1000) Config::loop();
    assert(configErases() == 0); // Idle for a day: no writes at all.
    Config::resetConfig();
    strcpy(globalConfig.wifi.ssid, "example");
    strcpy(globalConfig.wifi.ip, "192.168.100.200");
    assert(Config::saveConfig());
    assert(configErases() == 1);
    assert(Config::saveConfig());
    assert(configErases() == 1); // Identical settings submissions.
    globalConfig.mqtt.port = 1883;
    assert(Config::saveConfig());
    assert(configErases() == 2);
    memset(&globalConfig, 0, sizeof(globalConfig));
    Config::readConfig();
    assert(strcmp(globalConfig.wifi.ssid, "example") == 0 && globalConfig.mqtt.port == 1883);
    assert(strcmp(globalConfig.wifi.ip, "192.168.100.200") == 0);

    // Each kind of failure must leave a retry pending, even for the same data.
    for (int kind = 0; kind < 3; ++kind)
    {
        ++globalConfig.mqtt.port;
        failErase = kind == 0;
        failWrite = kind == 1;
        corruptWrite = kind == 2;
        assert(!Config::saveConfig());
        unsigned attempts = configErases();
        now += 59999;
        Config::loop();
        assert(configErases() == attempts);
        ++now;
        Config::loop();
        assert(configErases() == attempts + 1);
        uint16_t expected = globalConfig.mqtt.port;
        Config::readConfig();
        assert(globalConfig.mqtt.port == expected);
    }
}

static void testRestartPreparation()
{
    Module current;
    module = &current;
    current.failFlush = true;
    ++globalConfig.mqtt.port;
    unsigned before = configErases();
    assert(!Config::prepareRestart());
    assert(current.flushes == 1 && configErases() == before);
    current.failFlush = false;
    assert(Config::prepareRestart());
    assert(current.flushes == 2 && configErases() == before + 1);
    // Password setup can preserve the old embedded payload while migration is blocked.
    current.failSave = true;
    assert(!Config::saveConfig());
    uint8_t legacy[] = {0x10, 0x01, 0x20, 0x02};
    globalConfig.module_cfg.size = sizeof(legacy);
    memcpy(globalConfig.module_cfg.bytes, legacy, sizeof(legacy));
    strcpy(globalConfig.http.pass, "2$000102030405060708090a0b0c0d0e0f$6bb6535cc4adff6d2138573f125b9fea7135129df687ca37139a6720c5c8b8e0");
    assert(Config::saveConfig(false));
    assert(globalConfig.module_cfg.size == sizeof(legacy)
        && memcmp(globalConfig.module_cfg.bytes, legacy, sizeof(legacy)) == 0);
    auto saved = globalConfig;
    memset(&globalConfig, 0, sizeof(globalConfig));
    Config::readConfig();
    assert(memcmp(&globalConfig, &saved, sizeof(globalConfig)) == 0);
    unsigned saves = current.saves;
    current.failSave = false;
    now += 60000;
    Config::loop();
    assert(current.saves == saves + 1); // Global-only saves retain pending module retries.
    module = nullptr;
}

static void testPasswordRecovery()
{
    Module current;
    module = &current;
    current.failSave = true; // Recovery must not depend on module/LittleFS saves.
    Config::resetConfig();
    strcpy(globalConfig.wifi.ssid, "home-network");
    strcpy(globalConfig.wifi.pass, "wifi-password");
    strcpy(globalConfig.mqtt.server, "192.168.1.10");
    globalConfig.mqtt.port = 1883;
    strcpy(globalConfig.http.pass, "2$000102030405060708090a0b0c0d0e0f$6bb6535cc4adff6d2138573f125b9fea7135129df687ca37139a6720c5c8b8e0");
    globalConfig.module_cfg.size = 4;
    memcpy(globalConfig.module_cfg.bytes, "\x08\x0b\x20\x02", 4);
    assert(Config::saveConfig(false));
    auto original = globalConfig;
    auto previousFlash = flash;
    failErase = true;
    assert(!Config::resetWebPassword());
    assert(memcmp(&globalConfig, &original, sizeof(original)) == 0);
    assert(flash == previousFlash);

    assert(Config::resetWebPassword());
    memset(original.http.pass, 0, sizeof(original.http.pass));
    assert(memcmp(&globalConfig, &original, sizeof(original)) == 0);
    memset(&globalConfig, 0, sizeof(globalConfig));
    Config::readConfig();
    assert(memcmp(&globalConfig, &original, sizeof(original)) == 0);
    assert(current.saves == 0 && current.flushes == 0);
    assertConfigIsolation(previousFlash);
    unsigned before = configErases();
    assert(Config::resetWebPassword());
    assert(configErases() == before);
    module = nullptr;
    assert(Config::saveConfig());
    puts("PASS: password recovery persists only the cleared password, rolls back on failure and preserves other flash");
}

static void testRetiredConfigFields()
{
    flash.fill(0xff);
    // A valid older configuration can exceed the new schema's encoded maximum.
    Config::resetConfig();
    auto fill = [](char *value, size_t size) { memset(value, 'x', size - 1); };
    fill(globalConfig.wifi.ssid, sizeof(globalConfig.wifi.ssid));
    fill(globalConfig.wifi.pass, sizeof(globalConfig.wifi.pass));
    // The older schema allocated 15 bytes per IPv4 string, including NUL.
    fill(globalConfig.wifi.ip, 15);
    fill(globalConfig.wifi.sn, 15);
    fill(globalConfig.wifi.gw, 15);
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
    strcpy(http.pass, "secret");
    auto submessage = pb_ostream_from_buffer(oldHttp, sizeof(oldHttp));
    assert(pb_encode(&submessage, HttpConfigMessage_fields, &http));
    assert(pb_encode_tag(&submessage, PB_WT_STRING, 2));
    assert(pb_encode_string(&submessage, reinterpret_cast<const uint8_t *>("admin"), 5));
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
    assert(globalConfig.http.port == http.port);
    assert(strcmp(globalConfig.http.pass, http.pass) == 0);
    assert(globalConfig.debug.type == debug.type);
    assert(Config::saveConfig());
    assert((unsigned(flash[BACKUP + 2]) << 8 | flash[BACKUP + 3]) <= GlobalConfigMessage_size);
}

static void storeLegacyConfig()
{
    uint8_t payload[GlobalConfigMessage_size];
    auto stream = pb_ostream_from_buffer(payload, sizeof(payload));
    assert(pb_encode(&stream, GlobalConfigMessage_fields, &globalConfig));
    uint16_t len = stream.bytes_written, crc = Config::crc16(payload, len);
    uint8_t header[] = {0, GLOBAL_CFG_VERSION, uint8_t(len >> 8), uint8_t(len),
        uint8_t(crc >> 8), uint8_t(crc)};
    memcpy(flash.data() + CONFIG, header, sizeof(header));
    memcpy(flash.data() + CONFIG + 6, payload, len);
}

static void checkConfigurationCuts(const char *name, const GlobalConfigMessage &updated)
{
    const auto baseline = flash;
    Config::readConfig();
    const auto previous = globalConfig;
    globalConfig = updated;
    mutations = 0;
    assert(Config::saveConfig());
    const unsigned steps = mutations;
    const unsigned eraseWords = SPI_FLASH_SEC_SIZE / sizeof(uint32);
    assert(steps > eraseWords);
    unsigned tested = 0;
    for (unsigned cut = 0; cut < steps; ++cut)
    {
        // The first erased word invalidates the header for every later erase cut.
        // Sample its start/middle/end, then interrupt every program/commit word.
        if (cut < eraseWords - 1 && cut != 0 && cut != eraseWords / 2) continue;
        ++tested;
        flash = baseline;
        globalConfig = updated;
        mutations = 0;
        cutAfter = cut;
        assert(!Config::saveConfig(false));
        assert(powerOff);
        powerOff = false;
        cutAfter = -1;
        memset(&globalConfig, 0, sizeof(globalConfig));
        Config::readConfig();
        assert(memcmp(&globalConfig, &previous, sizeof(globalConfig)) == 0
            || memcmp(&globalConfig, &updated, sizeof(globalConfig)) == 0);
        assertConfigIsolation(baseline);
    }
    flash = baseline;
    globalConfig = updated;
    assert(Config::saveConfig());
    Config::readConfig();
    assert(memcmp(&globalConfig, &updated, sizeof(globalConfig)) == 0);
    printf("PASS: %u configuration power-cut points (%s), preserving Wi-Fi, MQTT, password and embedded runtime data\n", tested, name);
}

static void fixFooterCrc(unsigned address)
{
    auto data = flash.data() + address;
    size_t len = (unsigned(data[2]) << 8) | data[3];
    uint32_t crc = crc32(data + 4080, 8, crc32(data, len + 6));
    memcpy(data + 4088, &crc, sizeof(crc));
}

static void testConfigurationPowerCuts()
{
    module = nullptr;
    flash.fill(0xff);
    Config::resetConfig();
    auto updated = globalConfig;
    strcpy(updated.wifi.ssid, "home-network");
    strcpy(updated.wifi.pass, "wifi-password");
    strcpy(updated.mqtt.server, "192.168.1.10");
    updated.mqtt.port = 1883;
    strcpy(updated.http.pass, "2$000102030405060708090a0b0c0d0e0f$6bb6535cc4adff6d2138573f125b9fea7135129df687ca37139a6720c5c8b8e0");
    const uint8_t legacyRuntime[] = {0x08, 0x0b, 0xc8, 0x01, 0x7b, 0xd8, 0x01, 0x7b};
    updated.module_cfg.size = sizeof(legacyRuntime);
    memcpy(updated.module_cfg.bytes, legacyRuntime, sizeof(legacyRuntime));
    updated.module_crc = Config::crc16(updated.module_cfg.bytes, updated.module_cfg.size);
    updated.cfg_version = 1;
    checkConfigurationCuts("first save", updated);
    strcpy(updated.wifi.ntp, "ntp1.aliyun.com");
    checkConfigurationCuts("slot B to A", updated);
    ++updated.mqtt.port;
    updated.http.pass[strlen(updated.http.pass) - 1] = '1';
    checkConfigurationCuts("slot A to B", updated);

    // The backup starts out as arbitrary old OTA data, not a freshly erased area.
    flash.fill(0xa5);
    Config::resetConfig();
    storeLegacyConfig();
    checkConfigurationCuts("legacy migration", updated);
    auto baseline = flash;
    failRead = true;
    ++globalConfig.mqtt.port;
    assert(!Config::saveConfig() && flash == baseline);

    // Both slots are committed; corruption and malformed Protobuf fall back.
    flash.fill(0xff);
    globalConfig = updated;
    assert(Config::saveConfig()); // B, generation 1.
    auto previous = globalConfig;
    ++updated.mqtt.port;
    globalConfig = updated;
    assert(Config::saveConfig()); // A, generation 2.
    baseline = flash;
    flash[CONFIG + 4088] ^= 1;
    Config::readConfig();
    assert(memcmp(&globalConfig, &previous, sizeof(previous)) == 0);
    flash = baseline;
    memset(flash.data() + CONFIG + 4092, 0xff, 4); // Never committed.
    Config::readConfig();
    assert(memcmp(&globalConfig, &previous, sizeof(previous)) == 0);
    uint32_t marker;
    memcpy(&marker, baseline.data() + CONFIG + 4092, 4);
    for (unsigned bit = 0; bit < 32; ++bit)
    {
        if (marker & (1U << bit)) continue;
        flash = baseline;
        uint32_t torn = marker | (1U << bit); // One commit bit was never programmed.
        memcpy(flash.data() + CONFIG + 4092, &torn, 4);
        Config::readConfig();
        assert(memcmp(&globalConfig, &previous, sizeof(previous)) == 0);
    }
    flash = baseline;
    flash[CONFIG + 2] = 0; flash[CONFIG + 3] = 2;
    flash[CONFIG + 6] = 0x0a; flash[CONFIG + 7] = 0xff;
    uint16_t crc = Config::crc16(flash.data() + CONFIG + 6, 2);
    flash[CONFIG + 4] = crc >> 8; flash[CONFIG + 5] = crc;
    fixFooterCrc(CONFIG);
    checkConfigurationCuts("invalid newer Protobuf", updated);

    // Generation comparison also works across UINT32_MAX -> 0.
    uint32_t generation = UINT32_MAX;
    memcpy(flash.data() + CONFIG + 4084, &generation, 4); fixFooterCrc(CONFIG);
    --generation;
    memcpy(flash.data() + BACKUP + 4084, &generation, 4); fixFooterCrc(BACKUP);
    Config::readConfig();
    assert(memcmp(&globalConfig, &updated, sizeof(updated)) == 0);
    ++globalConfig.mqtt.port;
    auto wrapped = globalConfig;
    assert(Config::saveConfig());
    Config::readConfig();
    assert(memcmp(&globalConfig, &wrapped, sizeof(wrapped)) == 0);
    memcpy(&generation, flash.data() + BACKUP + 4084, 4);
    assert(generation == 0);
    puts("PASS: config CRC/commit/Protobuf fallback, read failures, generation wrap, and flash isolation");
}

static void testRuntimeStorage()
{
    assert(!RuntimeStorage::begin());
    assert(!RuntimeStorage::ready());
    assert(!LittleFS.autoFormat && LittleFS.formatCalls == 0);
    assert(!RuntimeStorage::saveRelay(5));
    assert(RuntimeStorage::initialize());
    assert(LittleFS.formatCalls == 1);
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
    // Upgrade UTC-day snapshots once, retain usage, and survive a reboot.
    EnergyState legacy = energy;
    legacy.utcDay = true;
    legacy.legacyDay = 100;
    legacy.day = 0;
    legacy.resolveLegacyDay(20000, 100, 365);
    assert(RuntimeStorage::saveEnergy(legacy));
    assert(RuntimeStorage::loadEnergy(restored) == RuntimeStorage::Result::Ok && restored.utcDay);
    restored.useLocalDay(20000, 20001);
    restored.updateDay(20001);
    assert(restored.today == legacy.today && restored.total == legacy.total);
    assert(RuntimeStorage::saveEnergy(restored));
    assert(RuntimeStorage::loadEnergy(restored) == RuntimeStorage::Result::Ok && !restored.utcDay);
    restored.useLocalDay(20000, 20001);
    restored.updateDay(20000); // Backward clock corrections must not clear usage.
    assert(restored.day == 20001 && restored.today == legacy.today);
    restored.updateDay(20002);
    assert(restored.today == 0 && restored.yesterday == legacy.today && restored.total == legacy.total);
    assert(RuntimeStorage::saveEnergy(energy));
    EnergyState invalid = energy;
    invalid.today = invalid.total + 1;
    assert(!RuntimeStorage::saveEnergy(invalid));
    LittleFS.corruptWrite = true;
    assert(!RuntimeStorage::saveRelay(3));
    assert(RuntimeStorage::loadRelay(state) == RuntimeStorage::Result::Ok && state == 5);
    LittleFS.failRename = true;
    assert(!RuntimeStorage::saveRelay(3));
    assert(RuntimeStorage::loadRelay(state) == RuntimeStorage::Result::Ok && state == 5);
    LittleFS.failRename = false;
    assert(RuntimeStorage::saveRelay(3));
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
}

int main()
{
    testConfiguration();
    testInitialProvisioning();
    testRestartPreparation();
    testPasswordRecovery();
    testRetiredConfigFields();
    testConfigurationPowerCuts();
    testRuntimeStorage();
    testCorruptRecordRecovery();
    testLegacyDecoding();
    puts("PASS: energy snapshots, event-driven settings, retries, isolation, legacy decoding");
}
