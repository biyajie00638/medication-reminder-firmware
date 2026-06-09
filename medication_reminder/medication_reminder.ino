/**
 * 服药提醒固件 - ESP32-S3 (ESP-BOX-3)
 * 
 * 功能：WiFi 连接 → 每 2 秒轮询服务器 → 有提醒时下载 WAV 并通过 I2S/ES8311 播放
 * 
 * 硬件：ESP-BOX-3 (ESP32-S3 + ES8311 codec)
 * 引脚：I2S MCLK=GPIO2, WS=GPIO45, BCLK=GPIO17, DIN=GPIO16, DOUT=GPIO15
 *       I2C SDA=GPIO8, SCL=GPIO18 (ES8311)
 *       PA 功放使能 = GPIO46
 * 
 * 依赖库（Arduino Library Manager 搜索安装）：
 *   - ESP32-audioI2S  (by schreibfaul1)
 *   - ArduinoJson     (by Benoit Blanchon, v6.x)
 * 
 * 板型设置：ESP32S3 Dev Module
 *   - Flash Size: 16MB
 *   - Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
 *   - PSRAM: OPI PSRAM
 */

#include "Arduino.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "Audio.h"        // ESP32-audioI2S 库
#include "ArduinoJson.h"
#include "driver/gpio.h"

// =====================================================================
// 配置区 - 请修改以下内容
// =====================================================================

// WiFi 凭据
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"

// 设备 MAC（固定写死，服务器用此 MAC 识别设备）
#define DEVICE_MAC      "fc:01:2c:c6:8a:38"

// 服务器地址
#define SERVER_HOST     "YOUR_SERVER_IP"
#define SERVER_PORT     8003

// 轮询间隔（毫秒）
#define POLL_INTERVAL_MS  2000

// =====================================================================
// ESP-BOX-3 引脚定义
// =====================================================================
#define I2S_MCLK   GPIO_NUM_2
#define I2S_WS     GPIO_NUM_45
#define I2S_BCLK   GPIO_NUM_17
#define I2S_DOUT   GPIO_NUM_15   // 数据输出到 ES8311（播放）
#define I2S_DIN    GPIO_NUM_16   // 数据输入（录音，本固件不用）

#define I2C_SDA    GPIO_NUM_8
#define I2C_SCL    GPIO_NUM_18

#define PA_CTRL    GPIO_NUM_46   // 功放使能，高电平有效

// =====================================================================
// 全局变量
// =====================================================================
Audio audio;
unsigned long lastPollTime = 0;
bool isPlaying = false;

// =====================================================================
// 工具函数
// =====================================================================

void enablePA(bool enable) {
    gpio_set_level(PA_CTRL, enable ? 1 : 0);
}

void initI2S() {
    // 使能 PA
    gpio_config_t pa_cfg = {};
    pa_cfg.intr_type    = GPIO_INTR_DISABLE;
    pa_cfg.mode         = GPIO_MODE_OUTPUT;
    pa_cfg.pin_bit_mask = (1ULL << PA_CTRL);
    gpio_config(&pa_cfg);
    enablePA(false);  // 初始关闭，播放时再开

    // 初始化 ESP32-audioI2S 库
    audio.setPinout(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
    audio.setVolume(15);  // 0-21
}

void connectWiFi() {
    Serial.printf("连接 WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi 连接成功! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi 连接失败，重启...");
        delay(3000);
        ESP.restart();
    }
}

// 轮询服务器，返回音频 URL（如果有待播放提醒）
String pollReminder() {
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        return "";
    }
    
    HTTPClient http;
    String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT 
                 + "/api/device/" + DEVICE_MAC + "/pending_reminder";
    
    http.begin(url);
    http.setTimeout(5000);
    int httpCode = http.GET();
    
    if (httpCode != 200) {
        Serial.printf("轮询失败: HTTP %d\n", httpCode);
        http.end();
        return "";
    }
    
    String payload = http.getString();
    http.end();
    
    // 解析 JSON: {"has_reminder": true, "id": 1, "audio_url": "http://..."}
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("JSON 解析失败: %s\n", err.c_str());
        return "";
    }
    
    bool hasReminder = doc["has_reminder"] | false;
    if (!hasReminder) {
        return "";  // 没有提醒
    }
    
    int reminderId = doc["id"] | 0;
    const char* audioUrl = doc["audio_url"] | "";
    
    Serial.printf("收到提醒 #%d: %s\n", reminderId, audioUrl);
    
    // 立即标记为已送达（防止重复播放）
    markDelivered(reminderId);
    
    return String(audioUrl);
}

// 标记提醒已送达
void markDelivered(int reminderId) {
    HTTPClient http;
    String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT 
                 + "/api/device/" + DEVICE_MAC + "/reminder_delivered?id=" + reminderId;
    http.begin(url);
    http.setTimeout(3000);
    http.GET();
    http.end();
    Serial.printf("提醒 #%d 已标记送达\n", reminderId);
}

// 播放音频 URL
void playAudio(const String& audioUrl) {
    Serial.printf("开始播放: %s\n", audioUrl.c_str());
    enablePA(true);   // 开启功放
    isPlaying = true;
    audio.connecttohost(audioUrl.c_str());
}

// =====================================================================
// ESP32-audioI2S 回调函数
// =====================================================================

void audio_info(const char *info) {
    Serial.printf("[Audio] %s\n", info);
}

void audio_eof_mp3(const char *info) {
    Serial.printf("[Audio] 播放完成: %s\n", info);
}

void audio_eof_stream(const char *info) {
    Serial.printf("[Audio] 流结束: %s\n", info);
    isPlaying = false;
    enablePA(false);   // 关闭功放节省电
}

void audio_error_on_id3info(const char *info) {
    Serial.printf("[Audio] 错误: %s\n", info);
    isPlaying = false;
    enablePA(false);
}

// =====================================================================
// Arduino 主程序
// =====================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== 服药提醒设备启动 ===");
    Serial.printf("设备 MAC: %s\n", DEVICE_MAC);
    
    initI2S();
    connectWiFi();
    
    Serial.println("初始化完成，开始轮询...");
}

void loop() {
    // ESP32-audioI2S 需要在每次 loop() 中调用 loop()
    audio.loop();
    
    // 播放中不轮询，避免打断
    if (isPlaying) {
        return;
    }
    
    // 检查是否到轮询时间
    unsigned long now = millis();
    if (now - lastPollTime < POLL_INTERVAL_MS) {
        return;
    }
    lastPollTime = now;
    
    // 轮询服务器
    String audioUrl = pollReminder();
    if (audioUrl.length() > 0) {
        playAudio(audioUrl);
    }
}
