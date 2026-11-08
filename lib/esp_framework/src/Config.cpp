#include <c_types.h>
#include <spi_flash.h>
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
#define EEPROM_PHYS_ADDR ((uintptr_t)(&_EEPROM_start) - 0x40200000)

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

void Config::resetConfig()
{
    Debug::AddInfo(PSTR("resetConfig . . . OK"));
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
    // The SDK requires word-aligned reads. The legacy payload starts at byte 6,
    // so read the complete sector and decode the unaligned payload from RAM.
    uint8_t *data = (uint8_t *)malloc(SPI_FLASH_SEC_SIZE);
    bool status = false;
    uint16_t len = 0;
    if (data && spi_flash_read(EEPROM_PHYS_ADDR, (uint32 *)data, SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK)
    {
        uint16_t version = (data[0] << 8 | data[1]);
        len = (data[2] << 8 | data[3]);
        uint16_t crc = (data[4] << 8 | data[5]);
        // Older schemas may contain retired fields beyond the current encoded
        // maximum. Bound reads by the sector, then let Protobuf skip them.
        if (version == GLOBAL_CFG_VERSION && len <= SPI_FLASH_SEC_SIZE - 6
            && crc16(data + 6, len) == crc)
        {
            memset(&globalConfig, 0, sizeof(GlobalConfigMessage));
            pb_istream_t stream = pb_istream_from_buffer(data + 6, len);
            status = pb_decode(&stream, GlobalConfigMessage_fields, &globalConfig);
            if (globalConfig.http.port == 0)
            {
                globalConfig.http.port = 80;
            }
        }
    }
    free(data);
    if (!status)
    {
        globalConfig.debug.type = 1;
        Debug::AddError(PSTR("readConfig . . . Error"));
        resetConfig();
    }
    else
    {
        if (module)
        {
            module->readConfig();
        }
        Debug::AddInfo(PSTR("readConfig       . . . OK Len: %d"), len);
    }
}

bool Config::saveConfig()
{
    if (writeConfig())
    {
        savePending = false;
        return true;
    }
    // Keep failed changes pending; never treat a failed write as committed.
    savePending = true;
    saveAt = millis() + 60000;
    return false;
}

bool Config::prepareRestart()
{
    if (module && !module->flushRuntime())
    {
        Debug::AddError(PSTR("Restart/OTA cancelled: runtime data not saved"));
        return false;
    }
    return saveConfig();
}

bool Config::writeConfig()
{
    if (module && !module->saveConfig())
    {
        return false;
    }
    uint8_t buffer[GlobalConfigMessage_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    if (!pb_encode(&stream, GlobalConfigMessage_fields, &globalConfig))
    {
        Debug::AddError(PSTR("saveConfig . . . Encode Error"));
        return false;
    }
    size_t len = stream.bytes_written;
    uint16_t crc = crc16(buffer, len);
    uint8_t header[6] = {uint8_t(GLOBAL_CFG_VERSION >> 8), uint8_t(GLOBAL_CFG_VERSION),
                         uint8_t(len >> 8), uint8_t(len), uint8_t(crc >> 8), uint8_t(crc)};
    uint8_t *data = (uint8_t *)malloc(SPI_FLASH_SEC_SIZE);
    if (!data)
    {
        return false;
    }
    if (spi_flash_read(EEPROM_PHYS_ADDR, (uint32 *)data, SPI_FLASH_SEC_SIZE) != SPI_FLASH_RESULT_OK)
    {
        free(data);
        Debug::AddError(PSTR("saveConfig . . . Read EEPROM Data Error"));
        return false;
    }
    // Compare the actual bytes as well as the checksum (CRC16 can collide).
    if (memcmp(data, header, sizeof(header)) == 0 && memcmp(data + 6, buffer, len) == 0)
    {
        free(data);
        return true;
    }
    memcpy(data, header, sizeof(header));
    memcpy(data + 6, buffer, len);
    bool status = spi_flash_erase_sector(EEPROM_PHYS_ADDR / SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK
        && spi_flash_write(EEPROM_PHYS_ADDR, (uint32 *)data, SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK
        && spi_flash_read(EEPROM_PHYS_ADDR, (uint32 *)data, SPI_FLASH_SEC_SIZE) == SPI_FLASH_RESULT_OK
        && memcmp(data, header, sizeof(header)) == 0 && memcmp(data + 6, buffer, len) == 0;
    free(data);
    if (!status)
    {
        Debug::AddError(PSTR("saveConfig . . . Write/Verify Error"));
        return false;
    }
    Debug::AddInfo(PSTR("saveConfig . . . OK Len: %d Crc: %d"), len, crc);
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
        Debug::AddError(PSTR("moduleReadConfig . . . Error %d %d %d"), globalConfig.cfg_version, version, globalConfig.module_cfg.size);
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
    else
    {
        Debug::AddInfo(PSTR("moduleReadConfig . . . OK Len: %d"), globalConfig.module_cfg.size);
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
            //Debug::AddInfo(PSTR("moduleSaveConfig . . . OK Len: %d"), len);
        }
    }
    return status;
}