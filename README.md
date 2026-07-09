# 用药提醒机器人（固件）

一个给长辈用的**离线语音用药提醒终端**的 ESP32 固件。设备每隔一段时间从服务端拉取当天的服药计划，
到点在 **LCD 屏显示药名/剂量** 并 **循环语音播报**，支持按键语音确认“已服药”；当家里 WiFi 变更时，
无需插线，服务端下发新账号后设备自动重连。

> 配套服务端仓库：[medication-server](https://github.com/biyajie00638/medication-server-)

---

## ✨ 功能特性

- 🖥️ **320×240 横屏 LCD** 显示时间、药名、剂量与提示（中文点阵，16×16 单色）
- 🔊 **语音播报**：每条提醒循环播放 5 次，使用服务端 edge-tts 中文语音
- ⏰ **定时拉取**：每 60 秒轮询服务端，自动匹配当前时段的服药计划
- ✅ **语音确认**：按顶部按键录音，上传服务端 ASR 识别“已服药”后标记完成
- 📶 **WiFi 远程重配置**：服务端网页改 WiFi 账号 → 设备下次轮询自动重连（连不上自动回退，不变砖）
- 💾 **NVS 持久化**：WiFi 凭据存于 ESP32 Flash，断电不丢
- 🔧 **时区正确**：服务端已固化 `Asia/Shanghai` 时区，提醒时间不再错 8 小时

---

## 🧰 硬件清单

| 部件 | 说明 |
|------|------|
| 主控 | **ESP32-S3-BOX-3**（乐鑫官方开发板，ESP32-S3） |
| 屏幕 | 板载 ILI9341，物理 320×240 横屏 |
| 音频 | ES8311（扬声器 DAC）+ ES7210（麦克风 ADC） |
| 存储 | 板载 PSRAM（中文点阵字库位于 PROGMEM） |

> 本项目针对 ESP32-S3-BOX-3 调试；其他 ESP32-S3 + ILI9341 + ES8311/ES7210 板卡可参考移植。

---

## 📁 目录结构

```
medication-reminder-firmware/
├── src/
│   ├── main.cpp            # 主固件（WiFi/NVS、轮询、LCD、音频、语音确认）
│   ├── config.h.example    # 配置模板（复制为 config.h 后填写）
│   ├── cjk_font.h          # 中文 16×16 点阵字库（离线生成，~1.6MB PROGMEM）
│   └── lcd_* / es8311_* / es7210_*  # 屏幕与音频驱动
├── releases/
│   └── v7.77/              # 预编译固件（bootloader/partitions/firmware.bin）
├── platformio.ini
└── README.md
```

---

## 🚀 快速开始

### 1. 准备工具
- 安装 [PlatformIO](https://platformio.org/)（VS Code 插件或命令行）
- 用 USB 连接 ESP32-S3-BOX-3

### 2. 配置私人凭据
```bash
cd src
cp config.h.example config.h
# 编辑 config.h，填入你的 WiFi / 服务端地址 / 设备 MAC
```

`config.h` 字段说明：

| 宏 | 含义 | 示例 |
|----|------|------|
| `WIFI_SSID` | 设备要连的 WiFi 名称 | `"my_home_wifi"` |
| `WIFI_PASS` | WiFi 密码 | `"password"` |
| `SERVER_BASE` | 服务端地址（IP:端口） | `"http://192.168.1.100:3000"` |
| `DEVICE_MAC` | 本机 MAC（设备背面或串口日志） | `"E8:F6:0A:A8:C3:BC"` |

> ⚠️ `config.h` 已被 `.gitignore` 忽略，**不会**随仓库公开。

### 3. 编译并烧录
```bash
# 编译
platformio run

# 烧录（按实际端口修改，Windows 常见 COM6）
platformio run --target upload --upload-port COM6
```

### 4. 关于固件二进制

本仓库**不提供预编译固件**。固件内含有你的 WiFi 密码、服务器地址等设备专属配置，
无法直接通用，也不应随公开仓库发布。请按上面的步骤用你自己的 `config.h` 本地编译：

- 编译产物在 `.pio/build/esp32s3box3/`，含 `bootloader.bin` / `partitions.bin` / `firmware.bin`
- 如需用 esptool 单独烧录（偏移同上）：

```bash
esptool.py --chip esp32s3 \
  --before=default_reset --after=hard_reset \
  write_flash --flash_mode dio --flash_size 16MB \
  0x0       .pio/build/esp32s3box3/bootloader.bin \
  0x8000    .pio/build/esp32s3box3/partitions.bin \
  0x10000   .pio/build/esp32s3box3/firmware.bin
```

> 🔒 **安全提示**：`src/config.h` 已被 `.gitignore` 忽略，提交前请确认其中不含真实凭据；
> 若曾把含密码的固件推到过别处，请及时修改对应 WiFi / 服务器密码。

---

## 🌐 服务端

固件需要配套的服务端（管理服药计划、TTS 语音、语音识别、设备 WiFi 下发）。
请参见 [medication-server](https://github.com/biyajie00638/medication-server-) 仓库，
按其中的说明用 Docker 部署。

服务端关键接口（设备侧自动调用，无需手动）：

| 接口 | 作用 |
|------|------|
| `GET /api/schedules?mac=...` | 拉取当前时段服药计划 |
| `PUT /api/schedules/:id/played` | 标记已播放 |
| `GET /api/audio/:name` | 下载 TTS 语音 |
| `POST /api/voice?mac=...` | 上传录音做 ASR + 意图识别 |
| `GET/POST /api/device/:mac/wifi/pending\|ack` | WiFi 远程重配置的拉取/回执 |

---

## 📶 WiFi 远程重配置

1. 打开服务端网页（默认 `http://<服务器IP>:3000`），登录后选设备
2. 在「📶 设备 WiFi 设置」卡片填入新的 SSID / 密码，点保存下发
3. 设备下次轮询（≤5 分钟）检测到新配置 → 断开重连
   - 连上：保存 NVS 并上报成功
   - 连不上：**自动回退原 WiFi**，不会变砖

---

## 🔖 版本

- `v7.77`：WiFi 凭据 NVS 持久化 + 服务端推送远程重配置（应用/重连/回执，带原账号回退）
- `v7.76`：320×240 横屏满屏（灰条彻底解决）+ 中文药名/剂量显示
- `v7.67`：LCD 时间/提醒显示 + 语音播报 + 自动日志

---

## 📄 开源协议

[MIT License](./LICENSE) — 可自由用于学习、改造与再分发，请保留版权声明。
