/**
 * 服药提醒极简固件 — ESP-BOX-3 (ESP32-S3 + ES8311)
 * 
 * 工作原理:
 *   1. WiFi 连接
 *   2. 每 2 秒 HTTP GET /api/device/{mac}/pending_reminder
 *   3. 有提醒 → 下载 WAV → 解析 → I2S+ES8311 播放
 *   4. 标记已送达
 * 
 * 无第三方音频库依赖，仅用 Arduino-ESP32 内置的 driver/i2s.h + Wire.h
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <driver/i2s.h>    // I2S 类型定义（setup 里用到了 I2S_NUM_0 等）
#include <ArduinoJson.h>

// ============ 配置 ============
const char* WIFI_SSID      = "lvgu-1";
const char* WIFI_PASSWORD  = "lvgu8888";
const char* SERVER_HOST    = "YOUR_SERVER_IP";
const int   SERVER_PORT    = 8003;
const char* DEVICE_MAC     = "e8:f6:0a:a8:c3:bc";
const int   POLL_INTERVAL  = 2000;          // 轮询间隔(ms)

// ============ ES8311 I2C (Wire1 总线, ESP-BOX-3 官方引脚) ============
#define ES8311_ADDR         0x18
#define I2C_SDA             8
#define I2C_SCL             18

// ============ I2S (ESP-BOX-3 官方引脚) ============
#define I2S_MCLK            2
#define I2S_BCK             17
#define I2S_WS              47    // = LRCK
#define I2S_DOUT            15
#define SAMPLE_RATE         24000
#define I2S_PORT            I2S_NUM_0

// ============ ES8311 初始化寄存器序列 ============
// 参考 datasheet 和 xiaozhi-esp32 代码
static void es8311_write_reg(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(ES8311_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

static bool es8311_init() {
    Wire1.begin(I2C_SDA, I2C_SCL, 100000);

    // 检查芯片是否存在
    Wire1.beginTransmission(ES8311_ADDR);
    if (Wire1.endTransmission() != 0) {
        Serial.println("[ES8311] Chip not found on I2C bus!");
        return false;
    }
    Serial.println("[ES8311] Found!");

    // Reset
    es8311_write_reg(0x00, 0x7F);
    es8311_write_reg(0x00, 0x80);
    es8311_write_reg(0x00, 0x00);
    delay(10);

    // Clock: PLL → MCLK=12.288MHz, fs=24kHz
    // Set LRCK divider (N) and BCLK divider
    es8311_write_reg(0x01, 0x3F);   // Power up VMID
    es8311_write_reg(0x02, 0x00);   // Power up various blocks
    es8311_write_reg(0x03, 0x28);   // MCLK from MCLK pin
    es8311_write_reg(0x04, 0x1F);   // Divider control
    es8311_write_reg(0x05, 0x00);   
    es8311_write_reg(0x06, 0x00);   // Slave mode
    es8311_write_reg(0x07, 0x0A);   // Fs = 24kHz (96*256=24576kHz → divider)
    es8311_write_reg(0x08, 0x0A);   
    es8311_write_reg(0x09, 0x00);   // ADC off
    es8311_write_reg(0x0A, 0x80);   // DAC powered up
    es8311_write_reg(0x0B, 0x00);   
    es8311_write_reg(0x0C, 0x02);   // I2S 16-bit
    es8311_write_reg(0x0D, 0x02);   
    es8311_write_reg(0x0E, 0x02);   
    es8311_write_reg(0x0F, 0x80);   

    // Analog: power up DAC
    es8311_write_reg(0x10, 0x00);
    es8311_write_reg(0x11, 0xBF);   // Power up
    es8311_write_reg(0x12, 0x80);   
    es8311_write_reg(0x13, 0x00);   
    es8311_write_reg(0x14, 0x12);   // Analog Vol
    es8311_write_reg(0x15, 0x00);
    es8311_write_reg(0x16, 0x40);   // HP driver
    es8311_write_reg(0x17, 0x80);
    es8311_write_reg(0x18, 0x00);

    // GPIO / general
    es8311_write_reg(0x19, 0x00);
    es8311_write_reg(0x1A, 0x00);
    es8311_write_reg(0x1B, 0x00);
    es8311_write_reg(0x1C, 0x00);
    es8311_write_reg(0x1D, 0x00);
    es8311_write_reg(0x1E, 0x00);
    es8311_write_reg(0x1F, 0x00);
    es8311_write_reg(0x20, 0x00);
    es8311_write_reg(0x21, 0x00);
    es8311_write_reg(0x22, 0x00);
    
    // ADC (disabled)
    es8311_write_reg(0x23, 0x00);
    es8311_write_reg(0x24, 0x00);
    es8311_write_reg(0x25, 0x00);
    es8311_write_reg(0x26, 0x00);
    es8311_write_reg(0x27, 0x00);
    es8311_write_reg(0x28, 0x00);
    es8311_write_reg(0x29, 0x00);

    // GPIO config
    es8311_write_reg(0x2A, 0x00);
    es8311_write_reg(0x2B, 0x00);

    // Test mode off
    es8311_write_reg(0x2C, 0x00);
    es8311_write_reg(0x2D, 0x00);
    es8311_write_reg(0x2E, 0x00);
    
    // DAC volume (0dB)
    es8311_write_reg(0x2F, 0x00);
    es8311_write_reg(0x30, 0x00);
    
    // Unmute
    es8311_write_reg(0x31, 0xC0);
    es8311_write_reg(0x32, 0x50);
    es8311_write_reg(0x33, 0x50);

    Serial.println("[ES8311] Initialized OK");
    return true;
}

// ============ I2S 初始化 ============
static bool i2s_init() {
    i2s_config_t cfg = {
        .mode               = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate         = SAMPLE_RATE,
        .bits_per_sample     = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format      = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags    = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count       = 8,
        .dma_buf_len         = 512,
        .use_apll            = true,
        .tx_desc_auto_clear  = true,
        .fixed_mclk          = 0,
        .mclk_multiple       = I2S_MCLK_MULTIPLE_256,
    };

    i2s_pin_config_t pins = {
        .mck_io_num    = I2S_MCLK,
        .bck_io_num    = I2S_BCK,
        .ws_io_num     = I2S_WS,
        .data_out_num  = I2S_DOUT,
        .data_in_num   = I2S_PIN_NO_CHANGE,
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Driver install failed: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Set pin failed: %d\n", err);
        return false;
    }

    Serial.println("[I2S] Initialized");
    return true;
}

// ============ 播放 WAV 数据 ============
static void play_wav(const uint8_t* data, size_t len) {
    if (len < 44) {
        Serial.println("[WAV] Too short");
        return;
    }

    // 解析 WAV 头
    uint32_t sample_rate = *(uint32_t*)(data + 24);
    uint16_t num_channels = *(uint16_t*)(data + 22);
    uint16_t bits_per_sample = *(uint16_t*)(data + 34);
    uint32_t data_size = *(uint32_t*)(data + 40);
    const uint8_t* pcm = data + 44;

    Serial.printf("[WAV] %lu Hz, %u-bit, %u ch, %lu bytes PCM\n",
                  sample_rate, bits_per_sample, num_channels, data_size);

    // 调整采样率（如果需要）
    if (sample_rate != SAMPLE_RATE) {
        i2s_set_sample_rates(I2S_PORT, sample_rate);
    }

    size_t written = 0;
    size_t chunk   = 1024;

    while (written < data_size) {
        size_t to_write = (data_size - written) < chunk ? (data_size - written) : chunk;
        size_t out_bytes;
        esp_err_t err = i2s_write(I2S_PORT, pcm + written, to_write, &out_bytes, portMAX_DELAY);
        if (err != ESP_OK) {
            Serial.printf("[I2S] Write error: %d\n", err);
            break;
        }
        written += out_bytes;
    }

    // 等播放完
    delay((data_size * 1000) / (sample_rate * num_channels * (bits_per_sample / 8)) + 100);

    // 恢复采样率
    if (sample_rate != SAMPLE_RATE) {
        i2s_set_sample_rates(I2S_PORT, SAMPLE_RATE);
    }
}

// ============ 轮询 + 播报 ============
static void poll_and_play() {
    HTTPClient http;
    String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT +
                 "/api/device/" + DEVICE_MAC + "/pending_reminder";

    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[Poll] HTTP %d\n", code);
        http.end();
        return;
    }

    String payload = http.getString();
    http.end();

    // 解析 JSON
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[Poll] JSON error: %s\n", err.c_str());
        return;
    }

    if (!doc["has_reminder"].as<bool>()) {
        return;  // 无提醒
    }

    const char* audio_url = doc["audio_url"];
    int reminder_id = doc["id"].as<int>();

    Serial.printf("[Reminder] #%d, downloading %s\n", reminder_id, audio_url);

    // 下载音频
    HTTPClient audio_http;
    audio_http.begin(audio_url);
    audio_http.setTimeout(10000);
    int audio_code = audio_http.GET();

    if (audio_code != 200) {
        Serial.printf("[Audio] HTTP %d\n", audio_code);
        audio_http.end();
        return;
    }

    // 读入内存
    int content_len = audio_http.getSize();
    uint8_t* wav_buf = (uint8_t*)malloc(content_len > 0 ? content_len : 256 * 1024);
    if (!wav_buf) {
        Serial.println("[Audio] Malloc failed");
        audio_http.end();
        return;
    }

    WiFiClient* stream = audio_http.getStreamPtr();
    size_t total = 0;
    while (stream->connected() && total < (size_t)(content_len > 0 ? content_len : 256 * 1024)) {
        int avail = stream->available();
        if (avail > 0) {
            int r = stream->read(wav_buf + total, avail);
            if (r > 0) total += r;
        }
    }
    audio_http.end();

    Serial.printf("[Audio] Downloaded %u bytes\n", total);

    // 播放
    if (total > 44) {
        play_wav(wav_buf, total);
    }

    free(wav_buf);

    // 标记已送达
    String delivered_url = String("http://") + SERVER_HOST + ":" + SERVER_PORT +
                           "/api/device/" + DEVICE_MAC + "/reminder_delivered?id=" + reminder_id;
    HTTPClient del_http;
    del_http.begin(delivered_url);
    del_http.GET();
    del_http.end();

    Serial.println("[Reminder] Marked delivered");
}

// ============ 主循环 ============
void setup() {
    Serial.begin(115200);
    delay(2000);  // 给 USB-serial 足够时间初始化
    Serial.println("\n\n=== Medication Reminder Firmware ===");
    Serial.printf("[INFO] Chip: %s Rev %d, Flash: %dMB, PSRAM: %s\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getFlashChipSize()/1024/1024,
                  psramFound() ? "Yes" : "No");
    Serial.printf("[INFO] Free heap: %d, MAC: %s\n", ESP.getFreeHeap(), DEVICE_MAC);

    // WiFi
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 40) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WiFi] Failed! Restarting...");
        delay(3000);
        ESP.restart();
    }

    // 音频硬件初始化
    if (!es8311_init()) {
        Serial.println("[Fatal] ES8311 init failed!");
    }
    if (!i2s_init()) {
        Serial.println("[Fatal] I2S init failed!");
    }

    // 输出引脚图（方便排查）
    Serial.printf("[Pins] I2C(SDA=%d,SCL=%d) I2S(MCLK=%d,BCK=%d,WS=%d,DOUT=%d)\n",
                  I2C_SDA, I2C_SCL, I2S_MCLK, I2S_BCK, I2S_WS, I2S_DOUT);

    Serial.println("[Ready] Polling server...");
}

void loop() {
    static unsigned long last_poll = 0;
    unsigned long now = millis();

    if (now - last_poll >= POLL_INTERVAL) {
        last_poll = now;

        if (WiFi.status() == WL_CONNECTED) {
            poll_and_play();
        } else {
            Serial.println("[WiFi] Disconnected, reconnecting...");
            WiFi.reconnect();
        }
    }

    delay(10);
}
