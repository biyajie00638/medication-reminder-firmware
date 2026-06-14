/*
 * ESP-BOX-3 Medication Reminder Firmware v3.1 (diagnostic)
 *
 * Based on v3.0 (proven working) with GPIO47 LED diagnostic blinks.
 * GPIO47 = backlight. Each init step blinks to show progress.
 *
 * Audio Path: ESP32-S3 I2S -> ES8311 DAC -> PA(GPIO46) -> Speaker
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <ArduinoJson.h>

// ============================================================
// PIN DEFINITIONS - ESP-BOX-3
// ============================================================
#define I2C_SDA         8
#define I2C_SCL         18
#define I2S_MCLK        2
#define I2S_BCLK        17
#define I2S_WS          45
#define I2S_DOUT        15
#define I2S_DIN         16
#define PA_ENABLE       46
#define BACKLIGHT       47    // LCD backlight (also used as diagnostic LED)
#define ES8311_ADDR     0x18

// ============================================================
// AUDIO CONFIG
// ============================================================
#define SAMPLE_RATE     16000
#define I2S_PORT        I2S_NUM_0
#define I2S_BITS        16

// ============================================================
// WIFI CONFIG
// ============================================================
#define WIFI_SSID       "byj"
#define WIFI_PASS       "REDACTED_WIFI_PASSWORD"

// ============================================================
// SERVER CONFIG (v3.0: port 3000, Basic auth)
// ============================================================
#define SERVER_BASE     "http://YOUR_SERVER_IP:3000"
#define DEVICE_MAC     "e8:f6:0a:a8:c3:bc"
#define SERVER_USER     "admin"
#define SERVER_PASS     "admin"
#define POLL_INTERVAL   60000   // 60 seconds

// ============================================================
// VOLUME (0-100)
// ============================================================
#define VOLUME          90

// ============================================================
// LOCAL DEDUP (prevent repeat reminders)
// ============================================================
#define MAX_PLAYED_IDS   20
static String g_played_ids[MAX_PLAYED_IDS];
static int   g_played_idx = 0;

static bool was_already_played(const char* id) {
    for (int i = 0; i < MAX_PLAYED_IDS; i++) {
        if (g_played_ids[i] == String(id)) return true;
    }
    return false;
}

static void mark_played_local(const char* id) {
    g_played_ids[g_played_idx] = String(id);
    g_played_idx = (g_played_idx + 1) % MAX_PLAYED_IDS;
}

// ============================================================
// GPIO47 DIAGNOSTIC BLINK (backlight)
// ============================================================
static void led_blink(int count, int on_ms = 200, int off_ms = 200) {
    pinMode(BACKLIGHT, OUTPUT);
    for (int i = 0; i < count; i++) {
        digitalWrite(BACKLIGHT, HIGH);
        delay(on_ms);
        digitalWrite(BACKLIGHT, LOW);
        delay(off_ms);
    }
}

// ============================================================
// ES8311 Register Helpers
// ============================================================
static bool es8311_write_reg(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(ES8311_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    return (Wire1.endTransmission() == 0);
}

static int es8311_read_reg(uint8_t reg) {
    Wire1.beginTransmission(ES8311_ADDR);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return -1;
    Wire1.requestFrom(ES8311_ADDR, (uint8_t)1);
    if (Wire1.available()) return Wire1.read();
    return -1;
}

// ============================================================
// ES8311 Initialization
// ============================================================
static bool es8311_init_codec() {
    Serial.println("[ES8311] Starting initialization...");
    bool ok = true;

    // Reset chip
    ok &= es8311_write_reg(0x45, 0x00);
    ok = true;  // Reg 0x45 is write-only, always returns ACK fail on read-back
    delay(50);

    // Clock & System
    ok &= es8311_write_reg(0x01, 0x30);
    ok &= es8311_write_reg(0x02, 0x10);
    ok &= es8311_write_reg(0x02, 0x00);
    ok &= es8311_write_reg(0x03, 0x10);
    ok &= es8311_write_reg(0x16, 0x24);
    ok &= es8311_write_reg(0x04, 0x10);
    ok &= es8311_write_reg(0x05, 0x00);
    ok &= es8311_write_reg(0x0B, 0x00);
    ok &= es8311_write_reg(0x0C, 0x00);

    // Digital Power
    ok &= es8311_write_reg(0x10, 0x1F);
    ok &= es8311_write_reg(0x11, 0x7F);

    // I2S Config - Slave mode
    ok &= es8311_write_reg(0x00, 0x80);

    // ADC
    ok &= es8311_write_reg(0x0D, 0x01);
    ok &= es8311_write_reg(0x0E, 0x02);
    ok &= es8311_write_reg(0x0F, 0x44);
    ok &= es8311_write_reg(0x15, 0x00);
    ok &= es8311_write_reg(0x1B, 0x0A);
    ok &= es8311_write_reg(0x1C, 0x6A);

    // DAC
    ok &= es8311_write_reg(0x12, 0x00);
    ok &= es8311_write_reg(0x13, 0x00);
    ok &= es8311_write_reg(0x14, 0x10);   // Soft mute off, DAC stereo

    // I2S Format - 16-bit I2S
    ok &= es8311_write_reg(0x09, 0x0C);
    ok &= es8311_write_reg(0x0A, 0x0C);

    // Enable all systems
    ok &= es8311_write_reg(0x01, 0x3F);

    // Volume: VOLUME 90 -> register ~0xAB
    uint8_t dac_vol = (uint8_t)((VOLUME * 0xBF) / 100);
    ok &= es8311_write_reg(0x32, dac_vol);
    ok &= es8311_write_reg(0x17, 0xBF);   // ADC volume: 0dB

    // Loopback OFF
    ok &= es8311_write_reg(0x44, 0x00);
    ok &= es8311_write_reg(0x37, 0x08);

    if (!ok) {
        Serial.println("[ES8311] ERROR: Register write failed!");
        return false;
    }

    // Verify
    int ver = es8311_read_reg(0x00);
    if (ver < 0) {
        Serial.println("[ES8311] ERROR: Cannot read reg 0x00!");
        return false;
    }
    Serial.printf("[ES8311] Reg 0x00 = 0x%02X (expect 0x80)\n", ver);
    Serial.println("[ES8311] Init OK!");
    return true;
}

// ============================================================
// I2S Initialization
// ============================================================
static bool i2s_init_driver() {
    Serial.println("[I2S] Initializing...");

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DIN
    };

    esp_err_t err;

    err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Install failed: %s\n", esp_err_to_name(err));
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Set pin failed: %s\n", esp_err_to_name(err));
        return false;
    }

    err = i2s_zero_dma_buffer(I2S_PORT);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Zero DMA failed: %s\n", esp_err_to_name(err));
        return false;
    }

    Serial.println("[I2S] Init OK!");
    return true;
}

// ============================================================
// PA Enable
// ============================================================
static void pa_enable(bool on) {
    pinMode(PA_ENABLE, OUTPUT);
    digitalWrite(PA_ENABLE, on ? HIGH : LOW);
}

// ============================================================
// Play test tone (1kHz sine, 2 seconds)
// ============================================================
static void play_test_tone(int duration_ms) {
    Serial.println("[AUDIO] Playing 1kHz test tone...");
    pa_enable(true);

    const double freq = 1000.0;
    const int samples = (SAMPLE_RATE * duration_ms) / 1000;
    const int16_t amplitude = 8000;

    const int chunk = 256;
    int16_t buf[chunk * 2];  // Stereo
    int played = 0;

    while (played < samples) {
        int n = min(chunk, samples - played);
        for (int i = 0; i < n; i++) {
            double t = (double)(played + i) / SAMPLE_RATE;
            int16_t val = (int16_t)(amplitude * sin(2.0 * PI * freq * t));
            buf[i * 2] = val;       // Left
            buf[i * 2 + 1] = val;   // Right
        }
        size_t bytes_written = 0;
        i2s_write(I2S_PORT, buf, n * 4, &bytes_written, portMAX_DELAY);
        played += n;
    }

    Serial.println("[AUDIO] Test tone done.");
}

// ============================================================
// Play WAV from buffer
// ============================================================
static void play_wav_data(const uint8_t* data, size_t len) {
    if (len < 44) {
        Serial.println("[WAV] Data too short");
        return;
    }

    uint16_t audio_fmt = data[20] | (data[21] << 8);
    uint16_t channels = data[22] | (data[23] << 8);
    uint16_t bits = data[34] | (data[35] << 8);

    if (audio_fmt != 1) {
        Serial.println("[WAV] Not PCM");
        return;
    }

    // Find "data" chunk
    size_t offset = 12;
    size_t audio_len = 0;
    while (offset < len - 8) {
        uint32_t chunk_size = data[offset+4] | (data[offset+5] << 8) |
                              (data[offset+6] << 16) | (data[offset+7] << 24);
        if (data[offset] == 'd' && data[offset+1] == 'a' &&
            data[offset+2] == 't' && data[offset+3] == 'a') {
            audio_len = chunk_size;
            offset += 8;
            break;
        }
        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;
    }

    if (audio_len == 0) {
        Serial.println("[WAV] No data chunk");
        return;
    }

    pa_enable(true);

    const uint8_t* ptr = data + offset;
    size_t remaining = min(audio_len, len - offset);
    const int buf_n = 512;
    int16_t out[buf_n * 2];
    size_t pos = 0;

    while (pos < remaining) {
        if (channels == 1 && bits == 16) {
            int n = min((int)(remaining - pos) / 2, buf_n);
            for (int i = 0; i < n; i++) {
                int16_t s = (int16_t)(ptr[pos + i*2] | (ptr[pos + i*2 + 1] << 8));
                out[i * 2] = s;
                out[i * 2 + 1] = s;
            }
            size_t bw = 0;
            i2s_write(I2S_PORT, out, n * 4, &bw, portMAX_DELAY);
            pos += n * 2;
        } else if (channels == 2 && bits == 16) {
            int n = min((int)(remaining - pos) / 4, buf_n);
            memcpy(out, ptr + pos, n * 4);
            size_t bw = 0;
            i2s_write(I2S_PORT, out, n * 4, &bw, portMAX_DELAY);
            pos += n * 4;
        } else {
            Serial.println("[WAV] Unsupported format");
            break;
        }
    }
    Serial.printf("[WAV] Playback done, %zu bytes\n", pos);
}

// ============================================================
// Base64 encode (for Basic auth)
// ============================================================
static String base64_encode(const String& str) {
    const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String encoded;
    int i = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];
    int in_len = str.length();
    const char* bytes_to_encode = str.c_str();

    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (i = 0; i < 4; i++) encoded += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        for (int j = 0; j < i + 1; j++) encoded += base64_chars[char_array_4[j]];
        while (i++ < 3) encoded += '=';
    }
    return encoded;
}

// ============================================================
// Add Basic auth header to HTTPClient
// ============================================================
static void http_add_auth(HTTPClient& http) {
    String auth = String(SERVER_USER) + ":" + String(SERVER_PASS);
    http.addHeader("Authorization", "Basic " + base64_encode(auth));
}

// ============================================================
// Download and play WAV from URL
// ============================================================
static bool download_and_play_wav(const String& url) {
    Serial.printf("[DL] Downloading: %s\n", url.c_str());
    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);

    if (!http.begin(url)) {
        Serial.println("[DL] begin failed");
        return false;
    }
    http_add_auth(http);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[DL] GET failed: %d\n", code);
        http.end();
        return false;
    }

    int content_len = http.getSize();
    if (content_len <= 0 || content_len > 1048576) {
        Serial.println("[DL] Invalid content length");
        http.end();
        return false;
    }

    uint8_t* buf = (uint8_t*)malloc(content_len);
    if (!buf) {
        Serial.println("[DL] malloc failed");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    int total = 0;
    unsigned long t0 = millis();
    while (total < content_len && millis() - t0 < 15000) {
        int avail = stream->available();
        if (avail) {
            total += stream->readBytes(buf + total, min(avail, content_len - total));
        } else {
            delay(10);
        }
    }
    http.end();

    if (total < content_len) {
        Serial.printf("[DL] Incomplete: %d/%d\n", total, content_len);
        free(buf);
        return false;
    }

    Serial.printf("[DL] Downloaded %d bytes, playing...\n", total);
    play_wav_data(buf, total);
    free(buf);
    return true;
}

// ============================================================
// Poll medication reminders
// ============================================================
static void poll_reminders() {
    String url = String(SERVER_BASE) + "/api/schedules?mac=" + String(DEVICE_MAC);
    Serial.printf("[POLL] %s\n", url.c_str());

    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);

    if (!http.begin(url)) {
        Serial.println("[POLL] begin failed");
        return;
    }
    http_add_auth(http);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[POLL] GET failed: %d\n", code);
        http.end();
        return;
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
        Serial.println("[POLL] JSON parse failed");
        return;
    }

    if (doc.is<JsonArray>()) {
        int pending = 0;
        for (JsonObject r : doc.as<JsonArray>()) {
            const char* id = r["id"] | "";
            const char* audio_url = r["audioUrl"] | "";
            const char* name = r["medicationName"] | "?";
            bool played = r["played"] | false;

            if (!played && strlen(audio_url) > 0) {
                // Local dedup check
                if (was_already_played(id)) {
                    Serial.printf("[POLL] Skip (already played): %s\n", name);
                    continue;
                }
                Serial.printf("[POLL] Playing: %s\n", name);
                led_blink(1, 100, 100);  // Single blink = playing

                download_and_play_wav(String(audio_url));
                mark_played_local(id);

                // Mark as played on server
                String mark_url = String(SERVER_BASE) + "/api/schedules/" +
                                  String(id) + "/played";
                HTTPClient http2;
                http2.setTimeout(5000);
                if (http2.begin(mark_url)) {
                    http_add_auth(http2);
                    http2.PUT("");
                    http2.end();
                }
                pending++;
            }
        }
        if (pending == 0) {
            Serial.println("[POLL] No pending reminders");
        }
    }
}

// ============================================================
// SETUP - with LED diagnostic at each step
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n\n====================================");
    Serial.println("  Medication Reminder v3.1 (diag)");
    Serial.println("====================================\n");

    // STEP 0: LED test - blink 5 times fast = "booting"
    led_blink(5, 100, 100);
    Serial.println("[STEP 0] LED test OK");

    // STEP 1: I2C
    Wire1.begin(I2C_SDA, I2C_SCL, 100000);
    delay(100);
    led_blink(1, 300, 200);
    Serial.println("[STEP 1] I2C init");

    // Scan I2C
    bool found = false;
    for (int a = 1; a < 127; a++) {
        Wire1.beginTransmission(a);
        if (Wire1.endTransmission() == 0) {
            Serial.printf("  I2C: 0x%02X\n", a);
            if (a == ES8311_ADDR) found = true;
        }
    }
    if (found) {
        Serial.println("[STEP 1] ES8311 found!");
        led_blink(1, 200, 200);
    } else {
        Serial.println("[STEP 1] ES8311 NOT FOUND!");
        led_blink(3, 500, 500);  // Error pattern
    }

    // STEP 2: PA enable
    pa_enable(true);
    led_blink(2, 200, 200);
    Serial.println("[STEP 2] PA enabled");

    // STEP 3: ES8311 init
    if (!es8311_init_codec()) {
        Serial.println("[STEP 3] ES8311 init FAILED");
        led_blink(3, 500, 500);  // Error pattern
    } else {
        Serial.println("[STEP 3] ES8311 init OK");
        led_blink(3, 200, 200);
    }

    // STEP 4: I2S
    if (!i2s_init_driver()) {
        Serial.println("[STEP 4] I2S init FAILED");
        led_blink(4, 500, 500);  // Error pattern
    } else {
        Serial.println("[STEP 4] I2S init OK");
        led_blink(4, 200, 200);
    }

    // STEP 5: Test tone
    Serial.println("[STEP 5] Playing test tone (2 sec)...");
    delay(500);
    play_test_tone(2000);
    led_blink(5, 300, 200);
    Serial.println("[STEP 5] Test tone done");

    // STEP 6: WiFi
    Serial.println("[STEP 6] Connecting WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[STEP 6] WiFi OK! IP: %s\n", WiFi.localIP().toString().c_str());
        led_blink(6, 200, 200);
    } else {
        Serial.println("\n[STEP 6] WiFi FAILED!");
        led_blink(6, 500, 500);  // Error pattern
    }

    Serial.println("\n[SETUP] Complete! Entering loop...\n");

    // Keep backlight on after init
    digitalWrite(BACKLIGHT, HIGH);
}

// ============================================================
// LOOP
// ============================================================
unsigned long last_poll = 0;

void loop() {
    unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED && (now - last_poll > POLL_INTERVAL || last_poll == 0)) {
        poll_reminders();
        last_poll = now;
    }

    delay(1000);
}
