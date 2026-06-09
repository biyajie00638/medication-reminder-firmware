# 服药提醒 ESP32-S3 固件

## 使用方法

### 1. 修改 WiFi 配置
打开 `medication_reminder.ino`，修改第 44-45 行：
```cpp
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"
```

### 2. 提交代码，GitHub Actions 自动编译
Push 到 `main` 分支后，Actions 自动编译，完成后下载 artifact。

### 3. 用乐鑫 Flash Download Tool 烧录

| 文件名 | 地址 |
|--------|------|
| `medication_reminder.ino.bootloader.bin` | 0x0000 |
| `medication_reminder.ino.partitions.bin` | 0x8000 |
| `boot_app0.bin`                          | 0xE000 |
| `medication_reminder.ino.bin`            | 0x10000 |

**注意：** 烧录前设备需进入下载模式（按住 BOOT 键，再按 RST 键，松开 RST 后松开 BOOT）

### 4. 测试
烧录完成后，打开串口监视器（115200 baud），设备会显示连接状态。

发送测试提醒：
```bash
curl -X POST http://YOUR_SERVER_IP:8003/api/reminder \
  -H "Content-Type: application/json" \
  -d '{"device_mac":"fc:01:2c:c6:8a:38","text":"该吃药了，请按时服用降压药"}'
```
设备应在 2 秒内自动播报语音。

## 硬件（ESP-BOX-3）

| 功能 | 引脚 |
|------|------|
| I2S MCLK | GPIO 2 |
| I2S WS (LRC) | GPIO 45 |
| I2S BCLK (SCK) | GPIO 17 |
| I2S DOUT | GPIO 15 |
| I2S DIN | GPIO 16 |
| I2C SDA (ES8311) | GPIO 8 |
| I2C SCL (ES8311) | GPIO 18 |
| PA 功放使能 | GPIO 46 |

## 依赖库
- [ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) v2.0.7
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) v6.21.3
