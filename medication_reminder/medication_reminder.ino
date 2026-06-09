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
#define PA_PIN              46    // 功放使能引脚
#define SAMPLE_RATE         24000
#define I2S_PORT            I2S_NUM_0

// ============ ES8311 初始化 — ESP-BSP 官方寄存器序列 ============
// 来源: espressif/esp-bsp components/es8311/es8311.c
// MCLK=6.144MHz (256×24kHz), fs=24kHz, 16-bit I2S, DAC-only slave mode
static void es8311_write_reg(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(ES8311_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

static uint8_t es8311_read_reg(uint8_t reg) {
    Wire1.beginTransmission(ES8311_ADDR);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    Wire1.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
    return Wire1.available() ? Wire1.read() : 0;
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

    // === 1. 复位序列 ===
    es8311_write_reg(0x00, 0x1F);   // 复位芯片
    delay(20);
    es8311_write_reg(0x00, 0x00);   // 清除复位
    es8311_write_reg(0x00, 0x80);   // 上电命令
    delay(10);

    // === 2. 时钟配置: MCLK pin, 不反相, 所有内部时钟使能 ===
    es8311_write_reg(0x01, 0x3F);

    // === 3. 时钟分频器: MCLK=6.144MHz, fs=24kHz ===
    // coeff_div 表: {6144000,24000, pre_div=1,pre_multi=0, adc_div=1,dac_div=1,
    //                  fs_mode=0, lrck_h=0,lrck_l=0xFF, bclk_div=4, adc_osr=0x10,dac_osr=0x10}
    es8311_write_reg(0x02, 0x00);   // pre_div=1→(1-1)<<5=0, pre_multi=0→0<<3=0
    es8311_write_reg(0x03, 0x10);   // fs_mode=0→0<<6=0, adc_osr=0x10
    es8311_write_reg(0x04, 0x10);   // dac_osr=0x10
    es8311_write_reg(0x05, 0x00);   // adc_div=1→0<<4=0, dac_div=1→0
    es8311_write_reg(0x06, 0x03);   // bclk_div=4→(4-1)=3
    es8311_write_reg(0x07, 0x00);   // lrck_h=0
    es8311_write_reg(0x08, 0xFF);   // lrck_l=0xFF

    // === 4. I2S 格式: 从模式 slave, 16-bit ===
    uint8_t reg00 = es8311_read_reg(0x00);
    es8311_write_reg(0x00, reg00 & 0xBF);  // 清除 bit6 → 从模式
    es8311_write_reg(0x09, 0x0C);   // SDP输入 16-bit: 3<<2=0x0C
    es8311_write_reg(0x0A, 0x0C);   // SDP输出 16-bit: 3<<2=0x0C

    // === 5. 模拟电路上电 ===
    es8311_write_reg(0x0D, 0x01);   // 上电模拟电路
    es8311_write_reg(0x0E, 0x02);   // 使能 PGA + ADC 调制器
    es8311_write_reg(0x12, 0x00);   // 上电 DAC
    es8311_write_reg(0x13, 0x10);   // 使能耳机驱动输出

    // === 6. DAC 均衡器旁路 ===
    es8311_write_reg(0x37, 0x08);   // 旁路 DAC EQ

    // === 7. 音量最大 (DAC_REG32, 0=静音, 255=最大) ===
    es8311_write_reg(0x20, 0xFF);   // 音量 100% → (100*256/100)-1 = 255 = 0xFF

    // === 8. 取消静音 (清除 DAC_REG31 bits 5,6) ===
    uint8_t reg1f = es8311_read_reg(0x1F);
    es8311_write_reg(0x1F, reg1f & ~(0x60));  // 清除 bit5, bit6

    Serial.println("[ES8311] Initialized OK (ESP-BSP sequence)");
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
    // PA 功放使能 (GPIO46) — 必须在 ES8311 初始化之前拉高
    pinMode(PA_PIN, OUTPUT);
    digitalWrite(PA_PIN, HIGH);
    Serial.println("[PA] Amplifier enabled (GPIO46)");

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
