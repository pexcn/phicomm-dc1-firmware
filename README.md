# Phicomm DC1 Firmware

斐讯 DC1 智能插排的本地控制固件，基于 ESP8266 系列芯片，无需原厂云服务。

## 功能

- 总开关及三路插座控制，支持上电记忆、互锁和过载保护。
- 整机电压、电流、功率及用电量统计；测量精度不作保证。
- MQTT 控制与 Home Assistant 自动发现。
- 适配手机、桌面的原生 WebUI，支持深浅主题和固件上传升级。

## 使用

1. 首次安装参考[拆机接线与烧录指南](docs/flashing.md)。固件从 [Releases](https://github.com/pexcn/phicomm-dc1-firmware/releases) 下载。
2. 无网络配置时，设备自动开启配网热点；连接后访问 `http://192.168.4.1`，首次设置管理密码后配置 Wi-Fi。总开关键长按 2～不足 10 秒后松手可重新配网。
3. 联网后访问设备 IP，输入管理密码登录，无需用户名。在“网络连接”配置 MQTT，按需开启 Home Assistant 自动发现。
4. 后续升级在“系统维护 → 固件升级”上传本项目新版 Release 的 `.bin` 固件。设备校验项目、硬件标识及 MD5，拒绝旧版无标识固件；这不是签名认证。

仅支持加盐 SHA-256 管理密码；旧格式需先在[过渡版本](https://github.com/pexcn/phicomm-dc1-firmware/tree/47f5b35)登录完成迁移。可在“系统维护”修改管理密码，侧栏底部退出登录。重启或连续 8 小时未访问后需重新登录；登录会话只存 RAM。当前使用局域网 HTTP，不提供传输加密。

忘记密码时，总开关键长按满 10 秒，LOGO 灯闪烁后松手。成功快闪三次，失败慢闪三次；仅清除管理密码和登录会话，其他配置、电量和供电状态保留。重新打开登录页设置新密码。

## 数据保存

- 配置变更时保存，相同内容不重复擦写；双槽交替写入，校验完成后提交，断电时保留完整的旧版或新版配置。
- 电量每 8 小时、跨日、清零及受控重启或升级前保存；意外断电可能丢失未保存增量。
- 需要断电记忆的开关状态，在最后一次变化 2 秒后合并保存。

首次使用或存储异常时，按“系统维护 → 用电统计与存储”的提示处理。挂载失败不会自动格式化；主动初始化可能清除历史记录。旧 Flash 记录会兼容迁移，之前仅存于 RAM 的电量无法恢复。

## 编译与开发

在 Python 虚拟环境中执行：

```sh
python -m pip install platformio==6.2.0
pio run
```

固件输出为 `output/dc1.bin`。Linux 下可运行 `python tests/host/run.py` 检查存储回归，需先完成编译并安装 GCC/G++。

1 MB Flash 中，配置槽位于 `0xFB000` 和 `0x74000`；原 LittleFS 与旧开关记录地址保留。固件文件上限为 464 KiB，编译与 OTA 均检查边界，避免覆盖配置备用槽。

- `src/`：DC1 设备逻辑。
- `lib/esp_framework/`：项目自带的配置、联网、Web、MQTT、日志和时钟模块，由 PlatformIO 自动编译。
- `web/`：原生 HTML、CSS、JavaScript，编译时自动压缩并嵌入固件，无外部库或 CDN。
- `platformio.ini`：构建配置及外部依赖。

修改 `.proto` 后，在同一环境中执行 `python scripts/generate-protos.py` 更新生成文件。

## 来源与声明

基于 [qlwz/esp_dc1](https://github.com/qlwz/esp_dc1)，许可证见 [LICENSE](LICENSE)。

> 原作者声明：仅供学习及个人非盈利使用，禁止商业用途、收费代刷等盈利行为；使用风险自行承担。

感谢 killadm、老妖、实验幼儿园小二班扛把子、Heller、巴山耗子、风中的 summer 等原项目贡献者。拆机图片来源见烧录指南。
