// Config.h

#ifndef _CONFIG_h
#define _CONFIG_h

#include <Ticker.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <pb.h>
#include "Arduino.h"
#include "GlobalConfig.pb.h"

#if PB_PROTO_HEADER_VERSION != 40
#error Regenerate this file with the current version of nanopb generator.
#endif

#define GLOBAL_CFG_VERSION 1 // 1 - 999

//#define WIFI_SSID "qlwz"     // WiFi ssid
//#define WIFI_PASS "" // WiFi 密码

//#define MQTT_SERVER "10.0.0.25"   // MQTTַ 地址
//#define MQTT_PORT 1883            // MQTT 端口
//#define MQTT_USER "mqtt"          // MQTT 用户名
//#define MQTT_PASS "" // MQTT 密码

#define MQTT_FULLTOPIC "%module%/%hostname%/%prefix%/" // MQTT 主题格式

#define WEB_LOG_SIZE 4000  // Max number of characters in weblog
#define BOOT_LOOP_OFFSET 5 // 开始恢复默认值之前的引导循环数 (0 = disable, 1..200 = 循环次数)

extern char UID[16];
extern char tmpData[512];
extern GlobalConfigMessage globalConfig;
extern uint32_t perSecond;
extern Ticker *tickerPerSecond;

class Config
{
private:
    static bool savePending;
    static unsigned long saveAt;
    static bool writeConfig();

public:
    static uint16_t crc16(uint8_t *ptr, uint16_t len);

    static void readConfig();
    static void resetConfig();
    static bool saveConfig();
    static bool prepareRestart();

    static bool moduleReadConfig(uint16_t version, uint16_t size, const pb_msgdesc_t *fields, void *dest_struct);
    static bool moduleSaveConfig(uint16_t version, uint16_t size, const pb_msgdesc_t *fields, const void *src_struct);

    static void loop();
};
#endif
