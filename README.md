# 用药提醒机器人 · 固件（ESP32-S3-BOX-3）

给**长辈 / 慢病老人**用的离线语音用药提醒终端固件。到点**亮屏显示药名/剂量**并**循环语音播报**，
老人按一下顶部「已服」键即确认；家里 WiFi 变更时无需插线，服务端下发新账号设备自动重连；
支持**固件远程 OTA** 与**零代码配网**。

> 配套服务端：[medication-server](https://github.com/biyajie00638/medication-server-)（管理服药计划、TTS 语音、机群、告警、OTA）

---

## ✨ 功能特性

- 🖥️ **320×240 横屏 LCD**：显示时间、药名、剂量与提示（中文 16×16 点阵，离线字库）
- 🔊 **语音播报**：每条提醒循环播放 5 次（后续催报仅 1 遍轻提醒），服务端 edge-tts 中文语音
- ✅ **物理「已服」确认**：按顶部键（GPIO1，active LOW）确认整轮待服药物；待确认队列每 5 分钟催报，确认仅在本机按键时生效
- 📢 **语音引导**：播报末尾提示「请按设备顶部的确认键，确认已服药」
- 📶 **零代码配网（SoftAP）**：WiFi 连不上，或 **长按 BOOT 键 3 秒**进入热点 `MedRemind-XXXX`，手机连上填家庭 WiFi 即联网
- 🔗 **扫码绑定家属**：**短按 BOOT 键（<3 秒）**屏幕显示绑定二维码，家属微信扫码即绑定
- 💾 **NVS 持久化 + 服务端推送重连**：WiFi 凭据存 Flash；服务端改 WiFi 后设备自动重连，连不上回退原账号（不变砖）
- 🛰️ **固件 OTA**：每 10 分钟查服务端，新版本**静默自动升级**（双分区 ota_0/ota_1 + otadata 防变砖）
- 🏭 **量产友好**：开机**自动读取硬件 MAC**（`WiFi.macAddress()`），一份编译产物即可烧录所有设备，无需逐台写 MAC

---

## 🧰 硬件清单

| 部件 | 说明 |
|------|------|
| 主控 | **ESP32-S3-BOX-3**（乐鑫官方开发板） |
| 屏幕 | 板载 ILI9341，物理 320×240 横屏 |
| 音频 | ES8311（扬声器 DAC）+ ES7210（麦克风 ADC） |
| 存储 | 板载 PSRAM（中文点阵字库位于 PROGMEM） |

按键：BOOT（GPIO0，烧录/配网/绑定）、顶部「已服」键（GPIO1）。

> 本项目针对 ESP32-S3-BOX-3 调试；其他 ESP32-S3 + ILI9341 + ES8311/ES7210 板卡可参考移植。

---

## 🚀 快速开始

### 1. 准备
- 安装 [PlatformIO](https://platformio.org/)（VS Code 插件或命令行）
- USB 连接 ESP32-S3-BOX-3

### 2. 配置
```bash
cd src
cp config.h.example config.h
# 编辑 config.h，至少填 SERVER_BASE（你的服务端地址）
```
| 宏 | 含义 | 示例 |
|----|------|------|
| `WIFI_SSID` / `WIFI_PASS` | 出厂预连 WiFi（可留空，首次开机用 SoftAP 配网） | `"my_home_wifi"` |
| `SERVER_BASE` | 服务端地址（IP:端口 或 HTTPS 域名） | `"https://med.biyajie00638.org"` |
| `DEVICE_MAC` | **可选**。不填则固件自动读取硬件 MAC（量产推荐留空） | `"E8:F6:0A:…"` |
| `WEB_BASE` | 绑定二维码跳转域名（需与微信合法域名一致） | `"https://med.biyajie00638.org"` |

> ⚠️ `src/config.h` 已被 `.gitignore` 忽略，**不会**随仓库公开。

### 3. 编译并烧录
```bash
platformio run
platformio run --target upload --upload-port COM6   # Windows 常见 COM6
```
编译产物在 `.pio/build/esp32s3box3/`（`bootloader.bin` / `partitions.bin` / `firmware.bin`）。
可用 esptool 单独烧录（偏移 0x0 / 0x8000 / 0x10000）。

---

## 🏭 量产 / 多设备运营（有人订购时）

1. **一份固件烧所有设备**：`config.h` 里只设 `SERVER_BASE`（与 `WEB_BASE`），`DEVICE_MAC` 留空。
   固件开机自动读 MAC，烧录同一 `firmware.bin` 到每台板子即可。
2. **寄到家里自助配网**：用户长按 BOOT 3 秒进热点，手机填家庭 WiFi → 设备联网并**自动在服务端注册**
   （后台机群视图出现该设备，初始名称=MAC）。
3. **运营方在后台改名 + 配方案**：在「设备机群」里把 MAC 改成可读备注（如「爸-卧室」），设置服药方案；
   让用户短按 BOOT 显二维码，家属扫码绑定微信，即可收漏服/离线告警。
4. **后续发版零运维**：服务端放新 `firmware.bin` + `sha256`，所有设备 ≤10 分钟静默 OTA 升级。

---

## 🛰️ OTA 发布流程

1. 改固件 `FIRMWARE_VERSION` 宏 → `platformio run` 生成 `firmware.bin`
2. 改服务端 `server.js` 的 `FIRMWARE_VERSION` 常量（**须与固件一致**）
3. 放 `firmware.bin` + `firmware.bin.sha256` 到服务端 `public/firmware/`，部署服务端
4. 设备下次轮询自动拉取刷写；双分区，失败自动回滚

---

## 🔐 隐私保护（固件侧）

- 固件轮询 `/api/schedules` 与确认 `/api/schedules/:id/confirm` 会携带**设备令牌**（`device_token`，首次联网由服务端下发并存入 NVS）。这样即使别人知道本机 MAC，也无法冒用拉取药名。
- 开机读取的 MAC 统一转**小写**，与服务端存储约定一致（避免大小写重复设备）。
- 药名仅在设备本地用于 TTS 播报；固件不向任何第三方暴露明文。

## 🔖 版本

- `v1.1.2`：MAC 统一小写（修复 1.1.0 起因 `WiFi.macAddress()` 大写导致计划不匹配）；设备令牌持久化并用于轮询/确认
- `v1.1.1`：轮询/确认携带设备令牌（响应头下发，NVS 持久化）
- `v1.1.0`：MAC 运行时自动读取（量产同二进制）；服务端设备改名接口；后台机群视图配套
- `v1.0.2`：固件 OTA 自更新（HTTPUpdate HTTPS 流式 + 双分区防变砖）
- `v1.0.1`：播报末尾「请按确认键」引导；一次按键确认整轮 + 轻催报优化
- `v1.0.0`：SoftAP 零代码配网 + BOOT 短按绑定二维码 + 物理「已服」确认键
- `v7.77`：320×240 横屏满屏 + 中文药名/剂量显示 + NVS WiFi 持久化

---

## 📄 开源协议

[MIT License](./LICENSE) — 可自由用于学习、改造与再分发，请保留版权声明。
