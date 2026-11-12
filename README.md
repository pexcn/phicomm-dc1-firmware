# Phicomm DC1 Firmware

斐讯 DC1 智能插排的 ESP8266 本地控制固件，无需原厂云服务。

## 功能

- 总开关与三路插座控制，上电记忆、互锁、过载保护。
- 电压、电流、功率和用电量统计。
- MQTT 控制与 Home Assistant 自动发现、手动 YAML 配置。
- 手机与桌面 WebUI，支持深浅主题、管理密码和 OTA 升级。

## 使用

1. 从 [Releases](https://github.com/pexcn/phicomm-dc1-firmware/releases) 下载固件，首次安装参考[烧录指南](docs/flashing.md)。
2. 连接设备配网热点，访问 `http://192.168.4.1`，设置管理密码并配置 Wi-Fi。
3. 联网后访问设备 IP。在“网络连接”配置 MQTT 和 Home Assistant。
4. 后续升级在“系统维护 → 固件升级”上传本项目新版 `.bin` 固件；旧版无标识固件不支持 OTA。

- **重新配网**：总开关键长按 2～不足 10 秒后松开。
- **忘记密码**：长按满 10 秒，LOGO 灯闪烁后松开，再打开登录页设置新密码。其他配置、电量和供电状态保留。

电量统计仅供参考，意外断电可能丢失未保存增量；存储异常按“系统维护 → 用电统计与存储”的提示处理，主动初始化可能清除历史记录。

管理界面使用局域网 HTTP，不提供传输加密。旧密码格式需先在[过渡版本](https://github.com/pexcn/phicomm-dc1-firmware/tree/47f5b35)登录迁移。

## 构建

在 Python 虚拟环境中执行：

```sh
python -m pip install platformio==6.2.0
pio run
```

固件输出为 `output/dc1.bin`。编译后可在 Linux 安装 GCC/G++，运行 `python tests/host/run.py` 执行关键回归测试。

修改 `.proto` 后执行 `python scripts/generate-protos.py` 更新生成文件。

## 来源

基于 [qlwz/esp_dc1](https://github.com/qlwz/esp_dc1)，许可证见 [LICENSE](LICENSE)。

> 原作者声明：仅供学习及个人非盈利使用，禁止商业用途、收费代刷等盈利行为；使用风险自行承担。

感谢原项目贡献者，拆机图片来源见烧录指南。
