// DC1.h
#ifndef _DC1_h
#define _DC1_h

#include "DC1Config.pb.h"
#include "Module.h"
#include "CAT9554.h"
#include "CSE7766.h"
#include "RuntimeStorage.h"

#define MODULE_CFG_VERSION 1002 //1001 - 1500

#define CAT9554_SDA_PIN 3
#define CAT9554_SCL_PIN 12
#define CAT9554_IRQ_PIN 4

#define CSE7766_RX_PIN 13
#define CSE7766_BAUDRATE 4800

#define LED_PIN 0       // 指示灯
#define LOGO_LED_PIN 14 // Logo指示灯

#define KEY_0_PIN 16 // 总开关
#define KEY_1_PIN 0  // 开关1
#define KEY_2_PIN 1  // 开关2
#define KEY_3_PIN 2  // 开关3

#define REL_0_PIN 7 // 总继电器
#define REL_1_PIN 6 // 继电器1
#define REL_2_PIN 5 // 继电器2
#define REL_3_PIN 4 // 继电器3

class DC1 : public Module
{
private:
    uint8_t operationFlag = 0; // 0每秒CSE7766
    EnergyState energy;
    uint8_t rememberedState = 0;
    bool relayStateValid = false;
    RuntimeStorage::Result relayResult = RuntimeStorage::Result::Missing;
    RuntimeStorage::Result energyResult = RuntimeStorage::Result::Missing;
    bool relaySavePending = false;
    bool runtimeMigrationPending = false;
    bool runtimeLoaded = false;
    void readRuntime(bool legacyValid);
    bool energySavePending = false;
    uint32_t energySaveAt = 0;
    uint32_t energyCheckpointAt = 0;
    void scheduleEnergySave(uint32_t delay = 0);
    bool saveRelay(bool recover = false);
    bool saveEnergy(bool recover = false);
    bool saveRuntime(bool recover = false);
    unsigned long relaySaveAt = 0;
    void scheduleRelaySave();

    CAT9554 *cat9554;
    CSE7766 *cse7766 = nullptr;
    char kWhtotalTime[20];
    void energyInit();
    bool energyClear();
    void energyUpdate();
    void energyUpdateToday();
    void energyMarginCheck();
    void energyMaxPower();
    void energyShow(bool isMqtt);

    char powerStatTopic[100];
    char energyTeleTopic[100];

    uint8_t btnGPIO[4] = {KEY_0_PIN, KEY_1_PIN, KEY_2_PIN, KEY_3_PIN};
    uint8_t relGPIO[4] = {REL_0_PIN, REL_1_PIN, REL_2_PIN, REL_3_PIN};

    // 按键
    uint8_t buttonDebounceTime = 50;
    uint16_t buttonLongPressTime = 2000; // 2000 = 2s
    uint8_t buttonTiming = 0;
    unsigned long buttonTimingStart[4] = {0, 0, 0, 0};
    uint8_t buttonAction[4] = {0, 0, 0, 0}; // 0 = 没有要执行的动作, 1 = 执行短按动作, 2 = 执行长按动作
    void checkButton(uint8_t ch);

    void httpDo(ESP8266WebServer *server);
    void httpSetting(ESP8266WebServer *server);
    void httpStorage(ESP8266WebServer *server);
    void httpHa(ESP8266WebServer *server);

    void logoLed();

    void reportPower();
    void reportEnergy();

public:
    DC1ConfigMessage config;
    uint8_t lastState = 0;
    uint8_t lastState2 = 0;
    uint8_t channels = 0;

    void init();
    String getModuleName() { return F("dc1"); }
    String getModuleCNName() { return F("DC1插线板"); }
    String getModuleVersion() { return F("2020.07.11.2000"); }
    bool moduleLed();

    void loop();
    void perSecondDo();

    void readConfig();
    void resetConfig();
    bool saveConfig();
    bool resetRuntime();
    bool flushRuntime();

    void mqttCallback(char *topic, char *payload, char *cmnd);
    void mqttConnected();
    void mqttDiscovery(bool isEnable = true);

    void httpAdd(ESP8266WebServer *server);
    String httpGetConfig();
    String httpGetStatus(ESP8266WebServer *server);

    void switchRelay(uint8_t ch, bool isOn, bool isSave = true);
};

#endif
