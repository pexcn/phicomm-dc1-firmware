
#include "DC1.h"
#include "Rtc.h"
#include "RuntimeStorage.h"
#include "Http.h"
#include "Util.h"

static const uint32_t ENERGY_SAVE_INTERVAL_MS = 8UL * 60 * 60 * 1000;

// 继承

void DC1::init()
{
    if (!runtimeLoaded) readRuntime(false);
    Led::init(LED_PIN, LOW);
    cat9554 = new CAT9554(CAT9554_SDA_PIN, CAT9554_SCL_PIN);
    cat9554->setIrqPin(CAT9554_IRQ_PIN);
    cat9554->setup();

    cse7766 = new CSE7766(CSE7766_RX_PIN, CSE7766_BAUDRATE);

    // 按键
    pinMode(KEY_0_PIN, INPUT_PULLDOWN_16);
    cat9554->pinMode(KEY_1_PIN, INPUT);
    cat9554->pinMode(KEY_2_PIN, INPUT);
    cat9554->pinMode(KEY_3_PIN, INPUT);

    // 继电器
    cat9554->pinMode(REL_0_PIN, OUTPUT);
    cat9554->pinMode(REL_1_PIN, OUTPUT);
    cat9554->pinMode(REL_2_PIN, OUTPUT);
    cat9554->pinMode(REL_3_PIN, OUTPUT);

    pinMode(LOGO_LED_PIN, OUTPUT);
    logoLed();

    strcpy(powerStatTopic, Mqtt::getStatTopic(F("power1")).c_str());
    strcpy(energyTeleTopic, Mqtt::getTeleTopic(F("energy")).c_str());

    channels = 4;
    bool safeOff = config.power_on_state >= 2 && !relayStateValid;
    for (uint8_t ch = 0; ch < channels; ch++)
    {
        // 0:开关通电时断开  1 : 开关通电时闭合  2 : 开关通电时状态与断电前相反  3 : 开关通电时保持断电前状态
        if (safeOff)
        {
            switchRelay(ch, false, false);
        }
        else if (config.power_on_state == 2)
        {
            switchRelay(ch, !bitRead(rememberedState, ch), false); // 开关通电时状态与断电前相反
        }
        else if (config.power_on_state == 3)
        {
            switchRelay(ch, bitRead(rememberedState, ch), false); // 开关通电时保持断电前状态
        }
        else
        {
            switchRelay(ch, config.power_on_state == 1, false); // 开关通电时闭合
        }
        // 总开关关时跳过其他
        if (!safeOff && ch == 0 && !bitRead(lastState, 0) && config.sub_kinkage != 0)
        {
            break;
        }
    }
    if (config.power_on_state < 2)
    {
        // If memory is enabled later, start from the fixed startup state.
        rememberedState = lastState;
        relayStateValid = true;
    }
    energyInit();
    energyCheckpointAt = millis() + ENERGY_SAVE_INTERVAL_MS;
}

bool DC1::moduleLed()
{
    if (WiFi.status() == WL_CONNECTED && (globalConfig.mqtt.port ==0 || Mqtt::mqttClient.connected()))
    {
        if (config.wifi_led == 0)
        {
            Led::on();
            return true;
        }
        else if (config.wifi_led == 1)
        {
            Led::off();
            return true;
        }
    }
    return false;
}

void DC1::loop()
{
    cse7766->loop();
    for (size_t ch = 0; ch < channels; ch++)
    {
        checkButton(ch);
    }

    if (bitRead(operationFlag, 0))
    {
        bitClear(operationFlag, 0);
        energyUpdate();
    }
    if (relaySavePending && (int32_t)(millis() - relaySaveAt) >= 0) saveRelay();
    if (energyResult != RuntimeStorage::Result::Invalid && !energySavePending
        && (int32_t)(millis() - energyCheckpointAt) >= 0)
        scheduleEnergySave();
    if (energySavePending && (int32_t)(millis() - energySaveAt) >= 0) saveEnergy();
    if (!relaySavePending && !energySavePending) runtimeMigrationPending = false;
}

void DC1::perSecondDo()
{
    bitSet(operationFlag, 0);
}

// 配置

void DC1::readConfig()
{
    bool valid = Config::moduleReadConfig(MODULE_CFG_VERSION, sizeof(DC1ConfigMessage), DC1ConfigMessage_fields, &config);
    readRuntime(valid);
}

void DC1::readRuntime(bool valid)
{
    runtimeLoaded = true;
    RuntimeStorage::begin();
    DC1LegacyStateMessage legacy = {};
    if (valid)
    {
        pb_istream_t stream = pb_istream_from_buffer(globalConfig.module_cfg.bytes, globalConfig.module_cfg.size);
        valid = pb_decode(&stream, DC1LegacyStateMessage_fields, &legacy);
    }
    relayResult = RuntimeStorage::loadRelay(rememberedState);
    relayStateValid = relayResult == RuntimeStorage::Result::Ok;
    if (relayResult != RuntimeStorage::Result::Ok)
    {
        // Only missing/unmounted files use legacy sources, never a corrupt record.
        if (relayResult != RuntimeStorage::Result::Invalid)
        {
            relayStateValid = RuntimeStorage::readLegacyRelay(rememberedState);
            // Proto3 omitted zero values; absence cannot establish trusted history.
            if (!relayStateValid && valid && legacy.last_state != 0 && legacy.last_state <= 15)
            {
                rememberedState = legacy.last_state & 15;
                relayStateValid = true;
            }
            if (relayStateValid)
            {
                scheduleRelaySave();
                runtimeMigrationPending = true;
            }
        }
        else Debug::AddError(PSTR("Relay snapshot invalid; legacy state not restored"));
    }
    energyResult = RuntimeStorage::loadEnergy(energy);
    if (energyResult != RuntimeStorage::Result::Ok)
    {
        if (energyResult != RuntimeStorage::Result::Invalid)
        {
            if (valid)
            {
                energy.utcDay = true;
                energy.today = legacy.energy_kWhtoday;
                energy.yesterday = legacy.energy_kWhyesterday;
                energy.total = legacy.energy_kWhtotal;
                energy.startTime = legacy.energy_kWhtotal_time;
                if (energy.total || energy.today || energy.yesterday || energy.startTime)
                    energy.legacyDay = legacy.energy_kWhdoy;
            }
            scheduleEnergySave();
            runtimeMigrationPending = true;
        }
        else Debug::AddError(PSTR("Energy snapshot invalid; legacy counters not restored"));
    }
    if (runtimeMigrationPending && RuntimeStorage::ready()) saveRuntime();
}

void DC1::resetConfig()
{
    Debug::AddInfo(PSTR("moduleResetConfig . . . OK"));
    memset(&config, 0, sizeof(DC1ConfigMessage));

    config.power_on_state = 3;
    config.power_mode = 0;
    config.logo_led = 0;
    config.wifi_led = 0;
    config.sub_kinkage = 2;
    config.energy_power_delta = 10;
    config.report_interval = 60;
    config.energy_max_power = 2300;
}

bool DC1::resetRuntime()
{
    runtimeLoaded = true;
    rememberedState = 0;
    relayStateValid = true;
    energy = EnergyState();
    RuntimeStorage::begin();
    relaySavePending = true;
    energySavePending = true;
    runtimeMigrationPending = true;
    relaySaveAt = energySaveAt = millis();
    return saveRuntime(true);
}

bool DC1::flushRuntime()
{
    if (cse7766)
    {
        cse7766->loop();
        cse7766->everySecond();
        energyUpdateToday();
    }
    return saveRuntime();
}

bool DC1::saveConfig()
{
    // Do not discard the legacy state until its independent copy is committed.
    if (runtimeMigrationPending)
    {
        bool relayOk = !relaySavePending || saveRelay();
        bool energyOk = !energySavePending || saveEnergy();
        if (!relayOk || !energyOk) return false;
        runtimeMigrationPending = false;
    }
    return Config::moduleSaveConfig(MODULE_CFG_VERSION, DC1ConfigMessage_size, DC1ConfigMessage_fields, &config);
}

// MQTT

void DC1::mqttCallback(char *topic, char *payload, char *cmnd)
{
    if (strlen(cmnd) == 6 && strncmp(cmnd, "power", 5) == 0) // strlen("power1") = 6
    {
        uint8_t ch = cmnd[5] - 49;
        if (ch < channels)
        {
            switchRelay(ch, (strcmp(payload, "on") == 0 ? true : (strcmp(payload, "off") == 0 ? false : !bitRead(lastState, ch))), true);
            return;
        }
    }
    else if (strcmp(cmnd, "report") == 0)
    {
        reportPower();
        reportEnergy();
    }
    else if (strcmp(cmnd, "clear") == 0)
    {
        energyClear();
    }
}

void DC1::mqttConnected()
{
    strcpy(powerStatTopic, Mqtt::getStatTopic(F("power1")).c_str());
    strcpy(energyTeleTopic, Mqtt::getTeleTopic(F("energy")).c_str());
    if (globalConfig.mqtt.discovery)
    {
        mqttDiscovery(true);
    }

    reportPower();
    reportEnergy();
}

void DC1::mqttDiscovery(bool isEnable)
{
    char topic[50];
    char message[500];

    String availability = Mqtt::getTeleTopic(F("availability"));
    char cmndTopic[100];
    strcpy(cmndTopic, Mqtt::getCmndTopic(F("power1")).c_str());
    for (size_t ch = 0; ch < channels; ch++)
    {
        sprintf(topic, PSTR("%s/switch/%s_%d/config"), globalConfig.mqtt.discovery_prefix, UID, (ch + 1));
        if (isEnable)
        {
            cmndTopic[strlen(cmndTopic) - 1] = ch + 49;           // 48 + 1 + ch
            powerStatTopic[strlen(powerStatTopic) - 1] = ch + 49; // 48 + 1 + ch
            sprintf(message,
                    PSTR("{\"name\":\"%s_%d\","
                         "\"cmd_t\":\"%s\","
                         "\"stat_t\":\"%s\","
                         "\"pl_off\":\"off\","
                         "\"pl_on\":\"on\","
                         "\"avty_t\":\"%s\","
                         "\"pl_avail\":\"online\","
                         "\"pl_not_avail\":\"offline\"}"),
                    UID, (ch + 1),
                    cmndTopic,
                    powerStatTopic,
                    availability.c_str());
            Mqtt::publish(topic, message, true);
        }
        else
        {
            Mqtt::publish(topic, "", true);
        }
    }

    String tims[] = {F("voltage"), F("current"), F("power"), F("apparent_power"), F("reactive_power"), F("factor"), F("total"), F("yesterday"), F("today"), F("starttime")};
    String tims2[] = {F("V"), F("A"), F("W"), F("VA"), F("VAr"), F(""), F("kWh"), F("kWh"), F("kWh"), F("")};
    String energy = Mqtt::getTeleTopic(F("energy"));
    for (size_t i = 0; i < 10; i++)
    {
        sprintf(topic, PSTR("%s/sensor/%s_%s/config"), globalConfig.mqtt.discovery_prefix, UID, tims[i].c_str());
        if (isEnable)
        {
            if (tims2[i].length() == 0)
            {
                sprintf(message,
                        PSTR("{\"name\":\"%s_%s\","
                             "\"stat_t\":\"%s\","
                             "\"val_tpl\":\"{{value_json.%s}}\"}"),
                        UID, tims[i].c_str(), energy.c_str(), tims[i].c_str());
            }
            else
            {
                sprintf(message,
                        PSTR("{\"name\":\"%s_%s\","
                             "\"stat_t\":\"%s\","
                             "\"val_tpl\":\"{{value_json.%s}}\","
                             "\"unit_of_meas\":\"%s\"}"),
                        UID, tims[i].c_str(), energy.c_str(), tims[i].c_str(), tims2[i].c_str());
            }
            Mqtt::publish(topic, message, true);
        }
        else
        {
            Mqtt::publish(topic, "", true);
        }
    }
    if (isEnable)
    {
        Mqtt::availability();
        reportPower();
        reportEnergy();
    }
}

// Http

void DC1::httpAdd(ESP8266WebServer *server)
{
    server->on(F("/dc1_do"), std::bind(&DC1::httpDo, this, server));
    server->on(F("/dc1_setting"), std::bind(&DC1::httpSetting, this, server));
    server->on(F("/dc1_storage"), HTTP_POST, std::bind(&DC1::httpStorage, this, server));
    server->on(F("/ha"), std::bind(&DC1::httpHa, this, server));
}

String DC1::httpGetStatus(ESP8266WebServer *server)
{
    String data;
    for (size_t ch = 0; ch < channels; ch++)
    {
        data += ",\"power" + String(ch + 1) + "\":";
        data += bitRead(lastState, ch) ? 1 : 0;
    }
    energyShow(false);
    data += String(tmpData);
    data += ",\"runtime_storage\":\"";
    data += RuntimeStorage::ready() ? "已挂载" : "未挂载，请初始化运行数据存储";
    data += "\",\"relay_record\":\"";
    data += relayResult == RuntimeStorage::Result::Invalid ? "记录损坏，已暂停保存，请恢复"
        : !relayStateValid ? "无可信历史，记忆或反转上电将保持断开"
        : relayResult != RuntimeStorage::Result::Ok ? "当前状态已知，尚无快照"
        : relaySavePending ? "有待保存的变化" : "快照有效";
    data += "\",\"energy_record\":\"";
    data += energyResult == RuntimeStorage::Result::Invalid ? "记录损坏，当前为临时统计，已暂停保存"
        : energyResult != RuntimeStorage::Result::Ok ? "尚无已保存快照"
        : energySavePending ? "快照有效，有待保存的变化" : "快照有效";
    data += "\"";
    data += ",\"runtime_ready\":" + String(RuntimeStorage::ready() ? 1 : 0);
    data += ",\"relay_needs_recovery\":" + String(relayResult == RuntimeStorage::Result::Invalid || !relayStateValid ? 1 : 0);
    data += ",\"energy_needs_recovery\":" + String(energyResult == RuntimeStorage::Result::Invalid ? 1 : 0);
    return data.substring(1);
}

String DC1::httpGetConfig()
{
    snprintf_P(tmpData, sizeof(tmpData),
        PSTR("{\"power_on_state\":%u,\"power_mode\":%u,\"logo_led\":%u,\"wifi_led\":%u,"
             "\"sub_kinkage\":%u,\"report_interval\":%u,\"energy_power_delta\":%u,\"energy_max_power\":%u}"),
        config.power_on_state, config.power_mode, config.logo_led, config.wifi_led,
        config.sub_kinkage, config.report_interval, config.energy_power_delta, config.energy_max_power);
    return String(tmpData);
}

void DC1::httpDo(ESP8266WebServer *server)
{
    if (!Http::checkAuth()) return;
    String c = server->arg(F("c"));
    if (c != F("1") && c != F("2") && c != F("3") && c != F("4"))
    {
        server->send_P(200, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"参数错误。\"}"));
        return;
    }
    uint8_t ch = c.toInt() - 1;
    if (ch >= channels)
    {
        server->send_P(200, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"继电器数量错误。\"}"));
        return;
    }
    String str = server->arg(F("do"));
    switchRelay(ch, (str == "on" ? true : (str == "off" ? false : !bitRead(lastState, ch))));

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1,\"msg\":\"操作成功\",\"data\":{"));
    server->sendContent(httpGetStatus(server));
    server->sendContent_P(PSTR("}}"));
}

void DC1::httpSetting(ESP8266WebServer *server)
{
    if (!Http::checkAuth()) return;
    if (server->hasArg(F("c")))
    {
        if (!energyClear())
        {
            server->send_P(500, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"电量已在内存清零，但保存失败；请检查运行数据存储，稍后重试。\"}"));
            return;
        }
        server->setContentLength(CONTENT_LENGTH_UNKNOWN);
        server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1,\"msg\":\"重置用电量成功。\",\"data\":{"));
        server->sendContent(httpGetStatus(server));
        server->sendContent_P(PSTR("}}"));
        return;
    }
    config.power_on_state = server->arg(F("power_on_state")).toInt();
    config.power_mode = server->arg(F("power_mode")).toInt();
    config.logo_led = server->arg(F("logo_led")).toInt();
    config.wifi_led = server->arg(F("wifi_led")).toInt();
    config.sub_kinkage = server->arg(F("sub_kinkage")).toInt();

    config.report_interval = server->arg(F("report_interval")).toInt();
    config.energy_power_delta = server->arg(F("energy_power_delta")).toInt();
    config.energy_max_power = server->arg(F("energy_max_power")).toInt();

    logoLed();
    if (config.power_on_state >= 2)
    {
        scheduleRelaySave();
    }
    else
    {
        relaySavePending = runtimeMigrationPending;
    }

    if (!Http::saveConfigOrError()) return;
    server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1,\"msg\":\"已经设置成功。\"}"));
}

void DC1::httpStorage(ESP8266WebServer *server)
{
    if (!Http::checkAuth()) return;
    String recover = server->arg(F("recover"));
    bool ok = false;
    if (RuntimeStorage::ready() && recover == F("relay")
        && (relayResult == RuntimeStorage::Result::Invalid || !relayStateValid))
    {
        rememberedState = lastState;
        relayStateValid = true;
        ok = saveRelay(true);
    }
    else if (RuntimeStorage::ready() && recover == F("energy")
        && energyResult == RuntimeStorage::Result::Invalid)
    {
        ok = energyClear();
    }
    else if (server->arg(F("initialize")) == F("1"))
    {
        if (RuntimeStorage::ready() && (relayResult == RuntimeStorage::Result::Invalid
            || energyResult == RuntimeStorage::Result::Invalid))
        {
            server->send_P(409, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"文件系统可用，请分别恢复损坏的开关记忆或电量记录。\"}"));
            return;
        }
        ok = RuntimeStorage::initialize() && saveRuntime();
    }
    else
    {
        server->send_P(400, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"请选择需要恢复的异常记录，或明确初始化存储。\"}"));
        return;
    }
    if (!ok)
    {
        server->send_P(500, PSTR("application/json"), PSTR("{\"code\":0,\"msg\":\"运行数据保存失败，原有记录未被主动清除，请检查设备后重试。\"}"));
        return;
    }
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1,\"msg\":\"操作已完成。\",\"data\":{"));
    server->sendContent(httpGetStatus(server));
    server->sendContent_P(PSTR("}}"));
}

void DC1::httpHa(ESP8266WebServer *server)
{
    char attachment[100];
    snprintf_P(attachment, sizeof(attachment), PSTR("attachment; filename=%s.yaml"), UID);

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->sendHeader(F("Content-Disposition"), attachment);
    server->send_P(200, PSTR("Content-Type: application/octet-stream"), "");

    String availability = Mqtt::getTeleTopic(F("availability"));
    char cmndTopic[100];
    strcpy(cmndTopic, Mqtt::getCmndTopic(F("power1")).c_str());
    server->sendContent(F("switch:\r\n"));
    for (size_t ch = 0; ch < channels; ch++)
    {
        cmndTopic[strlen(cmndTopic) - 1] = ch + 49;           // 48 + 1 + ch
        powerStatTopic[strlen(powerStatTopic) - 1] = ch + 49; // 48 + 1 + ch

        snprintf_P(tmpData, sizeof(tmpData),
                   PSTR("  - platform: mqtt\r\n"
                        "    name: \"%s_%d\"\r\n"
                        "    state_topic: \"%s\"\r\n"
                        "    command_topic: \"%s\"\r\n"
                        "    payload_on: \"on\"\r\n"
                        "    payload_off: \"off\"\r\n"
                        "    availability_topic: \"%s\"\r\n"
                        "    payload_available: \"online\"\r\n"
                        "    payload_not_available: \"offline\"\r\n\r\n"),
                   UID, ch + 1, powerStatTopic, cmndTopic, availability.c_str());
        server->sendContent_P(tmpData);
    }

    String tims[] = {F("voltage"), F("current"), F("power"), F("apparent_power"), F("reactive_power"), F("factor"), F("total"), F("yesterday"), F("today"), F("starttime")};
    String tims2[] = {F("V"), F("A"), F("W"), F("VA"), F("VAr"), F(""), F("kWh"), F("kWh"), F("kWh"), F("")};
    server->sendContent_P(PSTR("sensor:\r\n"));
    for (size_t i = 0; i < 10; i++)
    {
        if (tims2[i].length() == 0)
        {
            snprintf_P(tmpData, sizeof(tmpData),
                       PSTR("  - platform: mqtt\r\n"
                            "    name: \"%s_%s\"\r\n"
                            "    state_topic: \"%s\"\r\n"
                            "    value_template: \"{{value_json.%s}}\"\r\n\r\n"),
                       UID, tims[i].c_str(), energyTeleTopic, tims[i].c_str());
        }
        else
        {
            snprintf_P(tmpData, sizeof(tmpData),
                       PSTR("  - platform: mqtt\r\n"
                            "    name: \"%s_%s\"\r\n"
                            "    state_topic: \"%s\"\r\n"
                            "    value_template: \"{{value_json.%s}}\"\r\n"
                            "    unit_of_measurement: \"%s\"\r\n\r\n"),
                       UID, tims[i].c_str(), energyTeleTopic, tims[i].c_str(), tims2[i].c_str());
        }
        server->sendContent_P(tmpData);
    }
}

void DC1::logoLed()
{
    if (config.logo_led == 0)
    {
        digitalWrite(LOGO_LED_PIN, LOW);
    }
    else if (config.logo_led == 1)
    {
        digitalWrite(LOGO_LED_PIN, HIGH);
    }
    else if (config.logo_led == 2)
    {
        digitalWrite(LOGO_LED_PIN, bitRead(lastState, 0) ? LOW : HIGH);
    }
    else if (config.logo_led == 3)
    {
        digitalWrite(LOGO_LED_PIN, bitRead(lastState, 0) ? HIGH : LOW);
    }
}

bool DC1::saveRelay(bool recover)
{
    if (relayResult == RuntimeStorage::Result::Invalid && !recover)
    {
        relaySavePending = false;
        return false;
    }
    if (!relayStateValid)
    {
        relaySavePending = false;
        return true; // Never turn the unknown default into a trusted snapshot.
    }
    if (RuntimeStorage::saveRelay(rememberedState, recover))
    {
        relaySavePending = false;
        relayResult = RuntimeStorage::Result::Ok;
        return true;
    }
    uint8_t previous;
    relayResult = RuntimeStorage::loadRelay(previous);
    relaySavePending = relayResult != RuntimeStorage::Result::Invalid;
    relaySaveAt = millis() + 60000;
    Debug::AddError(PSTR("Relay save failed; storage may need initialization"));
    return false;
}

void DC1::scheduleEnergySave(uint32_t delay)
{
    energySavePending = true;
    energySaveAt = millis() + delay;
}

bool DC1::saveEnergy(bool recover)
{
    if (energyResult == RuntimeStorage::Result::Invalid && !recover)
    {
        energySavePending = false;
        return false;
    }
    if (RuntimeStorage::saveEnergy(energy, recover))
    {
        energySavePending = false;
        energyResult = RuntimeStorage::Result::Ok;
        energyCheckpointAt = millis() + ENERGY_SAVE_INTERVAL_MS;
        return true;
    }
    EnergyState previous;
    energyResult = RuntimeStorage::loadEnergy(previous);
    if (energyResult == RuntimeStorage::Result::Invalid) energySavePending = false;
    else scheduleEnergySave(60000);
    Debug::AddError(PSTR("Energy save failed; storage may need initialization"));
    return false;
}

bool DC1::saveRuntime(bool recover)
{
    bool relayOk = saveRelay(recover);
    bool energyOk = saveEnergy(recover);
    if (relayOk && energyOk) runtimeMigrationPending = false;
    return relayOk && energyOk;
}

void DC1::scheduleRelaySave()
{
    relaySavePending = true;
    // Merge a burst of channel changes; pending changes can be lost within 2s of power loss.
    relaySaveAt = millis() + 2000;
}

void DC1::switchRelay(uint8_t ch, bool isOn, bool isSave)
{
    if (ch >= channels)
    {
        Debug::AddInfo(PSTR("invalid channel: %d"), ch);
        return;
    }

    if (ch > 0 || (ch == 0 && config.sub_kinkage == 0))
    {
        if (!bitRead(lastState, 0) && isOn && config.sub_kinkage != 0)
        {
            if (config.sub_kinkage == 1 || !isSave)
            {
                isOn = false;
            }
            else if (config.sub_kinkage == 2)
            {
                switchRelay(0, true);
            }
        }

        if (isOn && config.power_mode == 1)
        {
            for (size_t ch2 = (config.sub_kinkage == 0 ? 0 : 1); ch2 < channels; ch2++)
            {
                if (ch2 != ch && bitRead(lastState, ch2))
                {
                    switchRelay(ch2, false, isSave);
                }
            }
        }
    }
    Debug::AddInfo(PSTR("Relay %d . . . %s"), ch + 1, isOn ? "ON" : "OFF");

    if (!cat9554->digitalWrite(relGPIO[ch], isOn ? HIGH : LOW))
    {
        Debug::AddError(PSTR("CAT9554 digitalWrite Error"));
        if (!cat9554->digitalWrite(relGPIO[ch], isOn ? HIGH : LOW))
        {
            Debug::AddError(PSTR("CAT9554 digitalWrite Error2"));
            return;
        }
    }

    bitWrite(lastState, ch, isOn);

    powerStatTopic[strlen(powerStatTopic) - 1] = ch + 49; // 48 + 1 + ch
    Mqtt::publish(powerStatTopic, isOn ? "on" : "off", globalConfig.mqtt.retain);

    if (isSave && (!relayStateValid || bitRead(rememberedState, ch) != isOn))
    {
        if (!relayStateValid) rememberedState = lastState;
        bitWrite(rememberedState, ch, isOn);
        relayStateValid = true;
        if (config.power_on_state >= 2)
        {
            scheduleRelaySave();
        }
    }

    if (ch == 0)
    {
        logoLed();
        if (isSave && config.sub_kinkage != 0)
        {
            for (size_t ch2 = 1; ch2 < channels; ch2++)
            {
                if (isOn)
                {
                    if (bitRead(rememberedState, ch2))
                    {
                        switchRelay(ch2, true, false);
                    }
                }
                else
                {
                    switchRelay(ch2, false, false);
                }
            }
        }
    }
}

void DC1::checkButton(uint8_t ch)
{
    bool buttonState = ch == 0 ? digitalRead(btnGPIO[ch]) : cat9554->digitalRead(btnGPIO[ch]);

    if (buttonState == 0)
    {
        if (!bitRead(buttonTiming, ch))
        {
            bitSet(buttonTiming, ch);
            buttonTimingStart[ch] = millis();
        }
        else
        { // buttonTiming = true
            if (millis() >= (buttonTimingStart[ch] + buttonDebounceTime))
            {
                buttonAction[ch] = 1;
            }
            if (millis() >= (buttonTimingStart[ch] + buttonLongPressTime))
            {
                buttonAction[ch] = 2;
            }
        }
    }
    else
    {
        bitClear(buttonTiming, ch);
        if (buttonAction[ch] != 0)
        {
            if (buttonAction[ch] == 1) // 执行短按动作
            {
                switchRelay(ch, !bitRead(lastState, ch), true);
            }
            else if (buttonAction[ch] == 2) // 执行长按动作
            {
                if (ch == 0)
                {
                    Wifi::setupWifiManager(false);
                }
            }
            buttonAction[ch] = 0;
        }
    }
}

void DC1::energyUpdate()
{
    if (Rtc::rtcTime.valid)
    {
        if (energy.startTime == 0)
        {
            energy.startTime = Rtc::utcTime;
            TIME_T tmpTime;
            Rtc::breakTime(energy.startTime, tmpTime);
            snprintf_P(kWhtotalTime, sizeof(kWhtotalTime), PSTR("%04d-%02d-%02d %02d:%02d:%02d"), tmpTime.year, tmpTime.month, tmpTime.day_of_month, tmpTime.hour, tmpTime.minute, tmpTime.second);
        }
        TIME_T utcCalendar;
        Rtc::breakTime(Rtc::utcTime, utcCalendar, false);
        energy.resolveLegacyDay(utcCalendar.days, utcCalendar.day_of_year,
            LEAP_YEAR(utcCalendar.year - 1 - 1970) ? 366 : 365);
        bool migrated = energy.utcDay;
        energy.useLocalDay(utcCalendar.days, Rtc::rtcTime.days);
        uint32_t previousDay = energy.day;
        energy.updateDay(Rtc::rtcTime.days);
        if (migrated || (previousDay && previousDay != energy.day))
        {
            scheduleEnergySave();
        }
    }
    if (cse7766->everySecond())
    {
        energyUpdateToday();
    }
    cse7766->Energy.daily = (float)energy.today / 100000;
    cse7766->Energy.total = (float)energy.total / 100000;
    energyMarginCheck();
}

void DC1::energyInit()
{
    cse7766->Energy.kWhtoday = 0;
    cse7766->Energy.kWhtoday_delta = 0;
    cse7766->Energy.daily = (float)(energy.today) / 100000;
    cse7766->Energy.total = (float)(energy.total) / 100000;

    strcpy(kWhtotalTime, "--");
    if (energy.startTime)
    {
        TIME_T tmpTime;
        Rtc::breakTime(energy.startTime, tmpTime);
        snprintf_P(kWhtotalTime, sizeof(kWhtotalTime), PSTR("%04d-%02d-%02d %02d:%02d:%02d"), tmpTime.year, tmpTime.month, tmpTime.day_of_month, tmpTime.hour, tmpTime.minute, tmpTime.second);
    }
}

bool DC1::energyClear()
{
    energy = EnergyState();
    energyInit();
    return saveEnergy(true);
}

void DC1::energyUpdateToday()
{
    if (cse7766->Energy.kWhtoday_delta >= 1000)
    {
        unsigned long delta = cse7766->Energy.kWhtoday_delta / 1000;
        cse7766->Energy.kWhtoday_delta %= 1000;
        energy.add(delta);
    }
}

void DC1::energyMarginCheck()
{
    if (cse7766->Energy.power_steady_counter)
    {
        cse7766->Energy.power_steady_counter--;
        return;
    }

    uint16_t energy_power_u = (uint16_t)(cse7766->Energy.active_power);

    if (config.energy_power_delta)
    {
        uint16_t delta = abs(cse7766->Energy.power_history[0] - energy_power_u);
        if (delta > 0)
        {
            if (config.energy_power_delta < 101)
            { // 1..100 = Percentage
                uint16_t min_power = (cse7766->Energy.power_history[0] > energy_power_u) ? energy_power_u : cse7766->Energy.power_history[0];
                if (0 == min_power)
                {
                    min_power++;
                } // Fix divide by 0 exception (#6741)
                if (((delta * 100) / min_power) > config.energy_power_delta)
                {
                    cse7766->Energy.power_delta = true;
                }
            }
            else
            { // 101..32000 = Absolute
                if (delta > (config.energy_power_delta - 100))
                {
                    cse7766->Energy.power_delta = true;
                }
            }
            if (cse7766->Energy.power_delta)
            {
                cse7766->Energy.power_history[1] = cse7766->Energy.active_power; // We only want one report so reset history
                cse7766->Energy.power_history[2] = cse7766->Energy.active_power;
            }
        }
    }

    cse7766->Energy.power_history[0] = cse7766->Energy.power_history[1]; // Shift in history every second allowing power changes to settle for up to three seconds
    cse7766->Energy.power_history[1] = cse7766->Energy.power_history[2];
    cse7766->Energy.power_history[2] = energy_power_u;

    if (config.report_interval > 0 && (perSecond % config.report_interval) == 0)
    {
        reportPower();
        cse7766->Energy.power_delta = true;
    }
    if (cse7766->Energy.power_delta)
    {
        cse7766->Energy.power_delta = false;
        reportEnergy();
    }
    if (config.energy_max_power)
    {
        DC1::energyMaxPower();
    }
}

void DC1::energyMaxPower()
{
    if (cse7766->Energy.active_power > config.energy_max_power)
    {
        if (!cse7766->Energy.mplh_counter)
        {
            cse7766->Energy.mplh_counter = MAX_POWER_HOLD;
        }
        else
        {
            cse7766->Energy.mplh_counter--;
            if (!cse7766->Energy.mplh_counter)
            {
                Debug::AddError(PSTR("MaxPowerReached: %d"), (uint16_t)cse7766->Energy.active_power);
                lastState2 = lastState;
                for (size_t ch = 0; ch < channels; ch++)
                {
                    if (bitRead(lastState, ch))
                    {
                        switchRelay(ch, false, false);
                    }
                }
                if (!cse7766->Energy.mplr_counter)
                {
                    cse7766->Energy.mplr_counter = MAX_POWER_RETRY + 1;
                }
                cse7766->Energy.mplw_counter = MAX_POWER_WINDOW;
                cse7766->Energy.mplv_counter = 0;
            }
        }
    }
    else if (lastState && (cse7766->Energy.active_power <= config.energy_max_power))
    {
        cse7766->Energy.mplh_counter = 0;
        cse7766->Energy.mplw_counter = 0;

        if (cse7766->Energy.mplv_counter++ == 60)
        {
            cse7766->Energy.mplv_counter = 0;
            cse7766->Energy.mplr_counter = 0;
        }
    }
    if (!lastState)
    {
        if (cse7766->Energy.mplw_counter)
        {
            cse7766->Energy.mplw_counter--;
        }
        else
        {
            if (cse7766->Energy.mplr_counter)
            {
                cse7766->Energy.mplr_counter--;
                if (cse7766->Energy.mplr_counter)
                {
                    Debug::AddError(PSTR("PowerMonitor ON"));
                    if (lastState2)
                    {
                        for (size_t ch = 0; ch < channels; ch++)
                        {
                            if (bitRead(lastState2, ch))
                            {
                                switchRelay(ch, true, false);
                            }
                        }
                        lastState2 = 0;
                    }
                }
                else
                {
                    Debug::AddInfo(PSTR("MaxPowerReachedRetry OFF"));
                }
            }
        }
    }
}

void DC1::energyShow(bool isMqtt)
{
    const uint8_t current_resolution = 3;
    const uint8_t voltage_resolution = 0;
    const uint8_t wattage_resolution = 0;
    const uint8_t energy_resolution = 3;

    float apparent_power = cse7766->Energy.apparent_power;
    if (isnan(apparent_power))
    {
        apparent_power = cse7766->Energy.voltage * cse7766->Energy.current;
    }
    if (apparent_power < cse7766->Energy.active_power)
    { // Should be impossible
        cse7766->Energy.active_power = apparent_power;
    }

    float power_factor = cse7766->Energy.power_factor;
    if (isnan(power_factor))
    {
        power_factor = (cse7766->Energy.active_power && apparent_power) ? cse7766->Energy.active_power / apparent_power : 0;
        if (power_factor > 1)
        {
            power_factor = 1;
        }
    }

    float reactive_power = cse7766->Energy.reactive_power;
    if (isnan(reactive_power))
    {
        reactive_power = 0;
        uint32_t difference = ((uint32_t)(apparent_power * 100) - (uint32_t)(cse7766->Energy.active_power * 100)) / 10;
        if ((cse7766->Energy.current > 0.005) && ((difference > 15) || (difference > (uint32_t)(apparent_power * 100 / 1000))))
        {
            // calculating reactive power only if current is greater than 0.005A and
            // difference between active and apparent power is greater than 1.5W or 1%
            reactive_power = (float)(Util::RoundSqrtInt((uint32_t)(apparent_power * apparent_power * 100) - (uint32_t)(cse7766->Energy.active_power * cse7766->Energy.active_power * 100))) / 10;
        }
    }

    char apparent_power_chr[16];
    char reactive_power_chr[16];
    char power_factor_chr[16];
    Util::dtostrfd(apparent_power, wattage_resolution, apparent_power_chr);
    Util::dtostrfd(reactive_power, wattage_resolution, reactive_power_chr);
    Util::dtostrfd(power_factor, 2, power_factor_chr);

    char voltage_chr[16];
    char current_chr[16];
    char active_power_chr[16];
    Util::dtostrfd(cse7766->Energy.voltage, voltage_resolution, voltage_chr);
    Util::dtostrfd(cse7766->Energy.current, current_resolution, current_chr);
    Util::dtostrfd(cse7766->Energy.active_power, wattage_resolution, active_power_chr);

    char energy_daily_chr[16];
    char energy_yesterday_chr[16];
    char energy_total_chr[16];
    Util::dtostrfd(cse7766->Energy.daily, energy_resolution, energy_daily_chr);
    Util::dtostrfd((float)energy.yesterday / 100000, energy_resolution, energy_yesterday_chr);
    Util::dtostrfd(cse7766->Energy.total, energy_resolution, energy_total_chr);

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("%s\"starttime\":\"%s\",\"total\":\"%s\",\"yesterday\":\"%s\",\"today\":\"%s\","
                    "\"voltage\":\"%s\",\"current\":\"%s\",\"power\":\"%s\","
                    "\"apparent_power\":\"%s\",\"reactive_power\":\"%s\",\"factor\":\"%s\"%s"),
               isMqtt ? PSTR("{") : PSTR(","),
               kWhtotalTime,
               energy_total_chr, energy_yesterday_chr, energy_daily_chr,
               voltage_chr, current_chr, active_power_chr,
               apparent_power_chr, reactive_power_chr, power_factor_chr,
               isMqtt ? PSTR("}") : PSTR(""));
}

void DC1::reportEnergy()
{
    energyShow(true);
    Mqtt::publish(energyTeleTopic, tmpData, globalConfig.mqtt.retain);
}

void DC1::reportPower()
{
    for (size_t ch = 0; ch < channels; ch++)
    {
        powerStatTopic[strlen(powerStatTopic) - 1] = ch + 49; // 48 + 1 + ch
        Mqtt::publish(powerStatTopic, bitRead(lastState, ch) ? "on" : "off", globalConfig.mqtt.retain);
    }
}
