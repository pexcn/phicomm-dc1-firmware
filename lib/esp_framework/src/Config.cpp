#include <c_types.h>
#include <spi_flash.h>
#include <coredecls.h>
#include "Module.h"

Module *module;
char UID[16];
char tmpData[512] = {0};
uint32_t perSecond;
Ticker *tickerPerSecond;
GlobalConfigMessage globalConfig;

bool Config::savePending = false;
unsigned long Config::saveAt = 0;

const uint16_t crcTalbe[] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400};

extern uint32_t _EEPROM_start; //See EEPROM.cpp
extern uint32_t _CONFIG_BACKUP_start;

/**
 * 计算Crc16
 */
uint16_t Config::crc16(uint8_t *ptr, uint16_t len)
{
    uint16_t crc = 0xffff;
    for (uint16_t i = 0; i < len; i++)
    {
        crc = crcTalbe[(ptr[i] ^ crc) & 15] ^ (crc >> 4);
        crc = crcTalbe[((ptr[i] >> 4) ^ crc) & 15] ^ (crc >> 4);
    }
    return crc;
}

namespace {
const uint32_t CONFIG_MAGIC = 0x32474643; // CFG2
const uint32_t CONFIG_COMMITTED = 0x54494d43;
struct ConfigFooter {
    uint32_t magic, generation, crc, committed;
};
static_assert(sizeof(ConfigFooter) == 16, "Configuration footer layout changed");
const size_t FOOTER_OFFSET = SPI_FLASH_SEC_SIZE - sizeof(ConfigFooter);
const size_t COMMIT_OFFSET = SPI_FLASH_SEC_SIZE - 4;
static_assert(GlobalConfigMessage_size + 6 <= FOOTER_OFFSET, "Configuration exceeds its slot");
struct ConfigSlot {
    bool readable = false, valid = false, legacy = false;
    uint32_t generation = 0;
};
uint32_t configAddress(unsigned slot)
{
    return uintptr_t(slot ? &_CONFIG_BACKUP_start : &_EEPROM_start) - 0x40200000;
}
ConfigFooter footerOf(const uint8_t *data)
{
    ConfigFooter footer;
    memcpy(&footer, data + FOOTER_OFFSET, sizeof(footer));
    return footer;
}
uint32_t recordCrc(const uint8_t *data, size_t len)
{
    return crc32(data + FOOTER_OFFSET, 8, crc32(data, len + 6));
}
ConfigSlot readSlot(unsigned slot, uint8_t *data, GlobalConfigMessage *decoded = nullptr)
{
    ConfigSlot result;
    result.readable = spi_flash_read(configAddress(slot), (uint32 *)data,
        SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK;
    if (!result.readable) return result;
    uint16_t version = (data[0] << 8) | data[1];
    uint16_t len = (data[2] << 8) | data[3];
    uint16_t crc = (data[4] << 8) | data[5];
    if (version != GLOBAL_CFG_VERSION || len > SPI_FLASH_SEC_SIZE - 6
        || Config::crc16(data + 6, len) != crc) return result;
    ConfigFooter footer = footerOf(data);
    if (footer.magic == CONFIG_MAGIC)
    {
        if (len > FOOTER_OFFSET - 6 || footer.committed != CONFIG_COMMITTED
            || footer.crc != recordCrc(data, len)) return result;
        result.generation = footer.generation;
    }
    else
    {
        // Only the original sector may contain a pre-upgrade configuration.
        if (slot != 0) return result;
        result.legacy = true;
    }
    result.valid = true;
    if (decoded)
    {
        memset(decoded, 0, sizeof(*decoded));
        pb_istream_t stream = pb_istream_from_buffer(data + 6, len);
        result.valid = pb_decode(&stream, GlobalConfigMessage_fields, decoded);
    }
    return result;
}
int newestSlot(const ConfigSlot *slots)
{
    if (!slots[0].valid) return slots[1].valid ? 1 : -1;
    if (!slots[1].valid) return 0;
    if (slots[0].legacy != slots[1].legacy) return slots[0].legacy ? 1 : 0;
    return int32_t(slots[1].generation - slots[0].generation) > 0 ? 1 : 0;
}
bool matchesSnapshot(const uint8_t *data, const uint8_t *header, const uint8_t *payload,
    size_t len, const ConfigFooter &expected, uint32_t committed)
{
    ConfigFooter footer = footerOf(data);
    return memcmp(data, header, 6) == 0 && memcmp(data + 6, payload, len) == 0
        && footer.magic == CONFIG_MAGIC && footer.generation == expected.generation
        && footer.crc == expected.crc && footer.committed == committed
        && footer.crc == recordCrc(data, len);
}
}

void Config::resetConfig()
{
    Debug::AddInfo(PSTR("CFG   Defaults restored"));
    memset(&globalConfig, 0, sizeof(GlobalConfigMessage));

#ifdef WIFI_SSID
    strcpy(globalConfig.wifi.ssid, WIFI_SSID);
#endif
#ifdef WIFI_PASS
    strcpy(globalConfig.wifi.pass, WIFI_PASS);

#endif
#ifdef MQTT_SERVER
    strcpy(globalConfig.mqtt.server, MQTT_SERVER);
#endif
#ifdef MQTT_PORT
    globalConfig.mqtt.port = MQTT_PORT;
#endif
#ifdef MQTT_USER
    strcpy(globalConfig.mqtt.user, MQTT_USER);
#endif
#ifdef MQTT_PASS
    strcpy(globalConfig.mqtt.pass, MQTT_PASS);
#endif
    globalConfig.mqtt.discovery = false;
    strcpy(globalConfig.mqtt.discovery_prefix, "homeassistant");

#ifdef MQTT_FULLTOPIC
    strcpy(globalConfig.mqtt.topic, MQTT_FULLTOPIC);
#endif
    globalConfig.http.port = 80;
    globalConfig.debug.type = 1;

    if (module)
    {
        module->resetConfig();
    }
}

void Config::readConfig()
{
    uint8_t *data = (uint8_t *)malloc(SPI_FLASH_SEC_SIZE);
    bool status = false;
    if (data)
    {
        ConfigSlot slots[2];
        for (unsigned i = 0; i < 2; ++i) slots[i] = readSlot(i, data, &globalConfig);
        int active = newestSlot(slots);
        if (active >= 0)
        {
            status = readSlot(active, data, &globalConfig).valid;
            if (!status && slots[1 - active].valid)
                status = readSlot(1 - active, data, &globalConfig).valid;
            if (status && globalConfig.http.port == 0) globalConfig.http.port = 80;
        }
    }
    free(data);
    if (!status)
    {
        globalConfig.debug.type = 1;
        Debug::AddError(PSTR("CFG   Read failed; using defaults"));
        resetConfig();
    }
    else
    {
        if (module)
        {
            module->readConfig();
        }
        Debug::AddInfo(PSTR("CFG   Configuration loaded"));
    }
}

bool Config::saveConfig(bool updateModule)
{
    if (writeConfig(updateModule))
    {
        if (updateModule) savePending = false;
        return true;
    }
    // Keep failed changes pending; never treat a failed write as committed.
    savePending = true;
    saveAt = millis() + 60000;
    return false;
}

bool Config::resetWebPassword()
{
    char previous[sizeof(globalConfig.http.pass)];
    memcpy(previous, globalConfig.http.pass, sizeof(previous));
    memset(globalConfig.http.pass, 0, sizeof(globalConfig.http.pass));
    // Preserve the module payload and bypass relay/energy storage during recovery.
    if (!saveConfig(false))
    {
        memcpy(globalConfig.http.pass, previous, sizeof(previous));
        return false;
    }
    return true;
}

bool Config::prepareRestart()
{
    if (module && !module->flushRuntime())
    {
        Debug::AddError(PSTR("SYS   Restart/OTA cancelled: runtime save failed"));
        return false;
    }
    return saveConfig();
}

bool Config::writeConfig(bool updateModule)
{
    // Access changes preserve the embedded legacy payload until runtime migration is safe.
    if (updateModule && module && !module->saveConfig())
    {
        return false;
    }
    uint8_t *data = (uint8_t *)malloc(SPI_FLASH_SEC_SIZE);
    if (!data) return false;
    // Decode old candidates before encoding the new snapshot, reusing the same
    // stack space. A malformed newer Protobuf must not displace the valid copy.
    union {
        GlobalConfigMessage decoded;
        uint8_t encoded[GlobalConfigMessage_size];
    } buffer;
    ConfigSlot slots[2];
    for (unsigned i = 0; i < 2; ++i) slots[i] = readSlot(i, data, &buffer.decoded);
    if (!slots[0].readable || !slots[1].readable)
    {
        free(data);
        Debug::AddError(PSTR("CFG   Flash read failed"));
        return false;
    }
    int active = newestSlot(slots);
    pb_ostream_t stream = pb_ostream_from_buffer(buffer.encoded, sizeof(buffer.encoded));
    if (!pb_encode(&stream, GlobalConfigMessage_fields, &globalConfig))
    {
        free(data);
        Debug::AddError(PSTR("CFG   Encode failed"));
        return false;
    }
    size_t len = stream.bytes_written;
    uint16_t crc = crc16(buffer.encoded, len);
    uint8_t header[6] = {uint8_t(GLOBAL_CFG_VERSION >> 8), uint8_t(GLOBAL_CFG_VERSION),
                         uint8_t(len >> 8), uint8_t(len), uint8_t(crc >> 8), uint8_t(crc)};
    if (active >= 0 && !readSlot(active, data).valid)
    {
        free(data);
        Debug::AddError(PSTR("CFG   Flash read failed"));
        return false;
    }
    // Compare the actual bytes as well as the checksum (CRC16 can collide).
    if (active >= 0 && memcmp(data, header, sizeof(header)) == 0
        && memcmp(data + 6, buffer.encoded, len) == 0)
    {
        free(data);
        return true;
    }
    memset(data, 0xff, SPI_FLASH_SEC_SIZE);
    memcpy(data, header, sizeof(header));
    memcpy(data + 6, buffer.encoded, len);
    ConfigFooter footer = {CONFIG_MAGIC,
        active < 0 || slots[active].legacy ? 1U : slots[active].generation + 1U,
        0, UINT32_MAX};
    memcpy(data + FOOTER_OFFSET, &footer, sizeof(footer));
    footer.crc = recordCrc(data, len);
    memcpy(data + FOOTER_OFFSET, &footer, sizeof(footer));
    const uint32_t target = configAddress(active == 1 ? 0 : 1);
    const size_t encodedSize = (sizeof(header) + len + 3) & ~size_t(3);
    uint32_t commit = CONFIG_COMMITTED;
    // Never erase the active slot. The final aligned word commits a fully
    // verified snapshot; until then the previous generation stays authoritative.
    bool status = spi_flash_erase_sector(target / SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK
        && spi_flash_write(target, (uint32 *)data, encodedSize) == SPI_FLASH_RESULT_OK
        && spi_flash_write(target + FOOTER_OFFSET, (uint32 *)(data + FOOTER_OFFSET),
            sizeof(ConfigFooter) - 4) == SPI_FLASH_RESULT_OK
        && spi_flash_read(target, (uint32 *)data, SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK
        && matchesSnapshot(data, header, buffer.encoded, len, footer, UINT32_MAX)
        && spi_flash_write(target + COMMIT_OFFSET, &commit, sizeof(commit)) == SPI_FLASH_RESULT_OK
        && spi_flash_read(target, (uint32 *)data, SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK
        && matchesSnapshot(data, header, buffer.encoded, len, footer, CONFIG_COMMITTED);
    free(data);
    if (!status)
    {
        Debug::AddError(PSTR("CFG   Flash write/verify failed"));
        return false;
    }
    Debug::AddInfo(PSTR("CFG   Configuration saved"));
    return true;
}

void Config::loop()
{
    if (savePending && (int32_t)(millis() - saveAt) >= 0)
    {
        saveConfig();
    }
}

bool Config::moduleReadConfig(uint16_t version, uint16_t size, const pb_msgdesc_t *fields, void *dest_struct)
{
    if (globalConfig.module_cfg.size == 0                                                                         // 没有数据
        || globalConfig.cfg_version != version                                                                    // 版本不一致
        || globalConfig.module_crc != Config::crc16(globalConfig.module_cfg.bytes, globalConfig.module_cfg.size)) // crc错误
    {
        Debug::AddError(PSTR("CFG   Module mismatch: stored=%u, expected=%u, size=%u"), globalConfig.cfg_version, version, globalConfig.module_cfg.size);
        if (module)
        {
            module->resetConfig();
        }
        return false;
    }
    memset(dest_struct, 0, size);
    pb_istream_t stream = pb_istream_from_buffer(globalConfig.module_cfg.bytes, globalConfig.module_cfg.size);
    bool status = pb_decode(&stream, fields, dest_struct);
    if (!status) // 解密失败
    {
        if (module)
        {
            module->resetConfig();
        }
    }
    return status;
}

bool Config::moduleSaveConfig(uint16_t version, uint16_t size, const pb_msgdesc_t *fields, const void *src_struct)
{
    uint8_t buffer[size];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    bool status = pb_encode(&stream, fields, src_struct);
    if (status)
    {
        size_t len = stream.bytes_written;
        uint16_t crc = Config::crc16(buffer, len);
        if (version != globalConfig.cfg_version || len != globalConfig.module_cfg.size
            || memcmp(buffer, globalConfig.module_cfg.bytes, len) != 0)
        {
            globalConfig.cfg_version = version;
            globalConfig.module_crc = crc;
            globalConfig.module_cfg.size = len;
            memcpy(globalConfig.module_cfg.bytes, buffer, len);
        }
    }
    return status;
}
