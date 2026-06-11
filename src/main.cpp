/*
 * ESP-BOX-3 Medication Reminder Firmware
 * 
 * Hardware: ESP32-S3-BOX-3 (ESP32-S3 + ES8311)
 * Framework: Arduino (PlatformIO)
 * 
 * Audio Path: ESP32-S3 I2S -> ES8311 DAC -> PA (GPIO46) -> Speaker
 * 
 * Pin Definitions (from ESP-BOX-3 schematic & xiaozhi-esp32 BSP):
 *   I2C SDA  = GPIO8   (Wire1)
 *   I2C SCL  = GPIO18  (Wire1)
 *   I2S MCLK = GPIO2
 *   I2S BCLK = GPIO17
 *   I2S WS   = GPIO45
 *   I2S DOUT = GPIO15  (to ES8311 DAC)
 *   I2S DIN  = GPIO16  (from ES7210 ADC, not used)
 *   PA EN    = GPIO46  (HIGH = speaker enabled)
 *   ES8311 I2C Address = 0x18
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <ArduinoJson.h>

// ============================================================
// PIN DEFINITIONS - ESP-BOX-3 (CRITICAL: verified from schematic)
// ============================================================
#define I2C_SDA         8
#define I2C_SCL         18
#define I2S_MCLK        2
#define I2S_BCLK        17
#define I2S_WS          45
#define I2S_DOUT        15
#define I2S_DIN         16
#define PA_ENABLE       46      // Power Amplifier enable (HIGH=on)
#define ES8311_ADDR     0x18   // 7-bit I2C address

// ============================================================
// AUDIO CONFIG
// ============================================================
#define SAMPLE_RATE     16000
#define I2S_PORT        I2S_NUM_0
#define I2S_BITS        16

// ============================================================
// WIFI CONFIG - Change these!
// ============================================================
#define WIFI_SSID       "byj"
#define WIFI_PASS       "REDACTED_WIFI_PASSWORD"

// ============================================================
// SERVER CONFIG
// ============================================================
#define SERVER_BASE     "http://YOUR_SERVER_IP:3001"
#define DEVICE_MAC     "e8:f6:0a:a8:c3:bc"
#define POLL_INTERVAL   30000   // 30s polling interval

// ============================================================
// VOLUME (0-100)
// ============================================================
#define VOLUME          80

// ============================================================
// ES8311 Register Write Helper
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
// ES8311 Full Initialization (register-level, from datasheet & 
// espressif/es8311 driver + ES8316 Linux driver reference)
// ============================================================
static bool es8311_init_codec() {
    Serial.println("[ES8311] Starting initialization...");
    
    bool ok = true;
    
    // Reset chip first
    ok &= es8311_write_reg(0x45, 0x00);   // Reset
    delay(50);                               // Wait for reset
    
    // ---- Clock & System Setup ----
    ok &= es8311_write_reg(0x01, 0x30);   // SYSTEM: CLK_MANAGER=1, ANALOG=0 (will enable later)
    ok &= es8311_write_reg(0x02, 0x10);   // CLK: MCLK=0, BCLK divider
    ok &= es8311_write_reg(0x02, 0x00);   // CLK: clear
    ok &= es8311_write_reg(0x03, 0x10);   // CLK: ADCCLK divider
    ok &= es8311_write_reg(0x16, 0x24);   // CLK: DACCLK divider
    ok &= es8311_write_reg(0x04, 0x10);   // CLK: MCLK source from MCLK pin
    ok &= es8311_write_reg(0x05, 0x00);   // CLK: ADC OSR
    ok &= es8311_write_reg(0x0B, 0x00);   // CLK: DSP config
    ok &= es8311_write_reg(0x0C, 0x00);   // CLK: ADC HPF
    
    // ---- Digital Power Supply Voltage Level ----
    ok &= es8311_write_reg(0x10, 0x1F);   // DIG_PDN: VDD=3.3V (matches ESP-BOX-3)
    ok &= es8311_write_reg(0x11, 0x7F);   // DIG_PDN: VDD=3.3V
    
    // ---- I2S Config ----
    ok &= es8311_write_reg(0x00, 0x80);   // CHIP_STA: Slave mode (bit7=0, bit6=ADC, bit5=DAC)
    // 0x80 = slave mode, ADC from I2S, DAC from I2S
    
    // ---- ADC Config ----
    ok &= es8311_write_reg(0x0D, 0x01);   // ADC: ramp rate
    ok &= es8311_write_reg(0x0E, 0x02);   // ADC: input selection (MIC1P/MIC1N)
    ok &= es8311_write_reg(0x0F, 0x44);   // ADC: VMID=0x44 (2*Vref/3)
    ok &= es8311_write_reg(0x15, 0x00);   // ADC: ALC config
    ok &= es8311_write_reg(0x1B, 0x0A);   // ADC: EQ/filter config
    ok &= es8311_write_reg(0x1C, 0x6A);   // ADC: EQ/filter config
    
    // ---- DAC Config ----
    ok &= es8311_write_reg(0x12, 0x00);   // DAC: config
    ok &= es8311_write_reg(0x13, 0x00);   // DAC: ramp rate
    ok &= es8311_write_reg(0x14, 0x10);   // DAC: soft mute off, DAC stereo
    
    // ---- I2S Format ----
    ok &= es8311_write_reg(0x09, 0x0C);   // I2S: 16-bit, I2S format
    ok &= es8311_write_reg(0x0A, 0x0C);   // I2S: 16-bit, I2S format
    
    // ---- Enable System ----
    ok &= es8311_write_reg(0x01, 0x3F);   // SYSTEM: CLK_MANAGER=1, ANALOG=1, ADC=1, DAC=1
    
    // ---- Volume ----
    // DAC volume: 0x00 = -95.5dB, 0xBF = 0dB, 0xFF = +31.5dB
    // For VOLUME=80 (out of 100), map to register value
    uint8_t dac_vol = (uint8_t)((VOLUME * 0xBF) / 100);
    ok &= es8311_write_reg(0x32, dac_vol); // DAC volume
    
    // ADC volume
    ok &= es8311_write_reg(0x17, 0xBF);   // ADC volume: 0dB
    
    // ---- ADC Input (disable mic for speaker-only) ----
    // Register 0x0E already set above for mic input; for speaker only,
    // we just need DAC output path working
    
    // ---- Loopback OFF (important!) ----
    ok &= es8311_write_reg(0x44, 0x00);   // Loopback off
    ok &= es8311_write_reg(0x37, 0x08);   // Additional config
    
    if (!ok) {
        Serial.println("[ES8311] ERROR: Some register writes failed!");
        return false;
    }
    
    // Verify communication by reading back a register
    int ver = es8311_read_reg(0x00);
    if (ver < 0) {
        Serial.println("[ES8311] ERROR: Cannot read back register 0x00!");
        return false;
    }
    Serial.printf("[ES8311] Register 0x00 = 0x%02X (expected 0x80)\n", ver);
    
    Serial.println("[ES8311] Initialization complete!");
    return true;
}

// ============================================================
// I2S Initialization (ESP-IDF driver)
// ============================================================
static bool i2s_init_driver() {
    Serial.println("[I2S] Initializing...");
    
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // Stereo for ES8311
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,  // Philips/I2S standard
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
        Serial.printf("[I2S] Driver install failed: %s\n", esp_err_to_name(err));
        return false;
    }
    
    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Set pin failed: %s\n", esp_err_to_name(err));
        return false;
    }
    
    // Enable MCLK output on GPIO2
    // For ESP32-S3, MCLK needs special handling
    err = i2s_mclk_gpio_select(I2S_PORT, I2S_MCLK);
    if (err != ESP_OK) {
        Serial.printf("[I2S] MCLK GPIO select failed: %s (non-fatal)\n", esp_err_to_name(err));
        // MCLK might work without explicit select on some IDF versions
    }
    
    err = i2s_zero_dma_buffer(I2S_PORT);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Zero DMA buffer failed: %s\n", esp_err_to_name(err));
        return false;
    }
    
    Serial.println("[I2S] Initialization complete!");
    return true;
}

// ============================================================
// PA (Power Amplifier) Enable
// ============================================================
static void pa_enable(bool on) {
    pinMode(PA_ENABLE, OUTPUT);
    digitalWrite(PA_ENABLE, on ? HIGH : LOW);
    Serial.printf("[PA] %s\n", on ? "ENABLED" : "DISABLED");
}

// ============================================================
// Play a test tone (1kHz sine wave) to verify audio output
// ============================================================
static void play_test_tone(int duration_ms) {
    Serial.println("[AUDIO] Playing test tone (1kHz)...");
    
    pa_enable(true);
    
    const double freq = 1000.0;
    const int samples = (SAMPLE_RATE * duration_ms) / 1000;
    const int16_t amplitude = 8000;  // Moderate volume
    
    // Generate and play sine wave in chunks
    const int chunk_samples = 256;
    int16_t buf[chunk_samples * 2];  // Stereo: L+R
    int played = 0;
    
    while (played < samples) {
        int n = min(chunk_samples, samples - played);
        for (int i = 0; i < n; i++) {
            double t = (double)(played + i) / SAMPLE_RATE;
            int16_t val = (int16_t)(amplitude * sin(2.0 * PI * freq * t));
            buf[i * 2] = val;      // Left channel
            buf[i * 2 + 1] = val;  // Right channel
        }
        size_t bytes_written = 0;
        i2s_write(I2S_PORT, buf, n * 4, &bytes_written, portMAX_DELAY);
        played += n;
    }
    
    Serial.println("[AUDIO] Test tone complete.");
}

// ============================================================
// Play WAV data from buffer
// ============================================================
static void play_wav_data(const uint8_t* data, size_t len) {
    if (len < 44) {
        Serial.println("[WAV] Data too short, not a valid WAV");
        return;
    }
    
    // Parse WAV header
    uint16_t audio_fmt = data[20] | (data[21] << 8);
    uint16_t channels = data[22] | (data[23] << 8);
    uint32_t sample_rate_wav = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
    uint16_t bits = data[34] | (data[35] << 8);
    
    Serial.printf("[WAV] Format=%d Ch=%d Rate=%lu Bits=%d\n", 
                  audio_fmt, channels, sample_rate_wav, bits);
    
    if (audio_fmt != 1) {  // Only PCM
        Serial.println("[WAV] Not PCM format, cannot play!");
        return;
    }
    
    // Find data chunk
    size_t data_offset = 12;  // Skip RIFF header
    size_t audio_data_len = 0;
    
    while (data_offset < len - 8) {
        char chunk_id[5] = {0};
        memcpy(chunk_id, data + data_offset, 4);
        uint32_t chunk_size = data[data_offset+4] | (data[data_offset+5] << 8) |
                              (data[data_offset+6] << 16) | (data[data_offset+7] << 24);
        
        if (strcmp(chunk_id, "data") == 0) {
            audio_data_len = chunk_size;
            data_offset += 8;
            break;
        }
        data_offset += 8 + chunk_size;
        // Align to even
        if (chunk_size & 1) data_offset++;
    }
    
    if (audio_data_len == 0) {
        Serial.println("[WAV] No data chunk found!");
        return;
    }
    
    pa_enable(true);
    
    const uint8_t* audio_ptr = data + data_offset;
    size_t remaining = min(audio_data_len, len - data_offset);
    
    // If sample rate matches, play directly; otherwise just play and accept distortion
    // For 16-bit mono, we need to convert to 16-bit stereo for I2S
    const int buf_samples = 512;
    int16_t out_buf[buf_samples * 2];
    size_t pos = 0;
    
    while (pos < remaining) {
        int samples_to_fill = buf_samples;
        if (channels == 1 && bits == 16) {
            // Mono 16-bit -> Stereo 16-bit
            int bytes_available = min((int)(remaining - pos), buf_samples * 2);
            int mono_samples = bytes_available / 2;
            samples_to_fill = mono_samples;
            
            for (int i = 0; i < mono_samples; i++) {
                int16_t sample = (int16_t)(audio_ptr[pos + i*2] | (audio_ptr[pos + i*2 + 1] << 8));
                out_buf[i * 2] = sample;      // Left
                out_buf[i * 2 + 1] = sample;  // Right
            }
            pos += mono_samples * 2;
        } else if (channels == 2 && bits == 16) {
            // Stereo 16-bit -> play directly
            int bytes_available = min((int)(remaining - pos), buf_samples * 4);
            int stereo_samples = bytes_available / 4;
            samples_to_fill = stereo_samples;
            memcpy(out_buf, audio_ptr + pos, stereo_samples * 4);
            pos += stereo_samples * 4;
        } else {
            Serial.printf("[WAV] Unsupported format: %d ch / %d bits\n", channels, bits);
            break;
        }
        
        size_t bytes_written = 0;
        i2s_write(I2S_PORT, out_buf, samples_to_fill * 4, &bytes_written, portMAX_DELAY);
    }
    
    Serial.printf("[WAV] Playback complete, %zu bytes played\n", pos);
}

// ============================================================
// HTTP WAV download and play
// ============================================================
static bool download_and_play_wav(const String& url) {
    Serial.printf("[HTTP] Downloading WAV: %s\n", url.c_str());
    
    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);
    
    if (!http.begin(url)) {
        Serial.println("[HTTP] Failed to begin connection");
        return false;
    }
    
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[HTTP] GET failed: %d\n", code);
        http.end();
        return false;
    }
    
    int content_len = http.getSize();
    Serial.printf("[HTTP] Content-Length: %d\n", content_len);
    
    // Read all data into buffer (max 1MB for safety)
    if (content_len <= 0 || content_len > 1048576) {
        Serial.println("[HTTP] Invalid content length");
        http.end();
        return false;
    }
    
    uint8_t* buf = (uint8_t*)malloc(content_len);
    if (!buf) {
        Serial.println("[HTTP] Failed to allocate buffer");
        http.end();
        return false;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    int total_read = 0;
    unsigned long start_ms = millis();
    
    while (total_read < content_len && millis() - start_ms < 15000) {
        int available = stream->available();
        if (available) {
            int read = stream->readBytes(buf + total_read, min(available, content_len - total_read));
            total_read += read;
        } else {
            delay(10);
        }
    }
    
    http.end();
    
    if (total_read < content_len) {
        Serial.printf("[HTTP] Incomplete download: %d / %d\n", total_read, content_len);
        free(buf);
        return false;
    }
    
    Serial.printf("[HTTP] Downloaded %d bytes, playing...\n", total_read);
    play_wav_data(buf, total_read);
    free(buf);
    return true;
}

// ============================================================
// Poll medication schedules
// ============================================================
static void poll_medication_reminders() {
    String url = String(SERVER_BASE) + "/api/schedules?mac=" + String(DEVICE_MAC);
    Serial.printf("[POLL] Checking schedules: %s\n", url.c_str());
    
    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);
    
    if (!http.begin(url)) {
        Serial.println("[POLL] HTTP begin failed");
        return;
    }
    
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[POLL] GET failed: %d\n", code);
        http.end();
        return;
    }
    
    String response = http.getString();
    http.end();
    
    // Parse JSON response
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        Serial.printf("[POLL] JSON parse failed: %s\n", err.c_str());
        return;
    }
    
    // Check for pending reminders
    if (doc.is<JsonArray>()) {
        for (JsonObject reminder : doc.as<JsonArray>()) {
            const char* audio_url = reminder["audioUrl"] | "";
            const char* med_name = reminder["medicationName"] | "unknown";
            bool played = reminder["played"] | false;
            
            if (!played && strlen(audio_url) > 0) {
                Serial.printf("[POLL] Playing reminder for: %s\n", med_name);
                download_and_play_wav(String(audio_url));
                
                // Mark as played
                String mark_url = String(SERVER_BASE) + "/api/schedules/" + 
                                  reminder["id"].as<String>() + "/played";
                HTTPClient http2;
                http2.begin(mark_url);
                http2.PUT("");
                http2.end();
            }
        }
    }
}

// ============================================================
// Print all ES8311 registers (debug)
// ============================================================
static void es8311_dump_regs() {
    Serial.println("[ES8311] Register dump:");
    for (int reg = 0; reg <= 0x44; reg++) {
        int val = es8311_read_reg(reg);
        if (val >= 0) {
            Serial.printf("  REG[0x%02X] = 0x%02X\n", reg, val);
        } else {
            Serial.printf("  REG[0x%02X] = READ_FAIL\n", reg);
        }
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);  // Wait for serial
    
    Serial.println("\n\n====================================");
    Serial.println("  Medication Reminder - ESP-BOX-3");
    Serial.println("  Firmware v2.0 (ES8311 fix)");
    Serial.println("====================================\n");
    
    // ---- Step 1: Initialize I2C (Wire1 for ESP-BOX-3) ----
    Serial.println("[INIT] Step 1: I2C (Wire1)");
    Wire1.begin(I2C_SDA, I2C_SCL, 100000);
    delay(100);
    
    // Scan I2C bus to verify ES8311 is present
    Serial.println("[INIT] Scanning I2C bus...");
    bool found_es8311 = false;
    for (int addr = 1; addr < 127; addr++) {
        Wire1.beginTransmission(addr);
        if (Wire1.endTransmission() == 0) {
            Serial.printf("  I2C device found at 0x%02X\n", addr);
            if (addr == ES8311_ADDR) found_es8311 = true;
        }
    }
    
    if (!found_es8311) {
        Serial.println("[INIT] ERROR: ES8311 NOT FOUND at 0x18! Check wiring!");
        // Don't return - try anyway
    } else {
        Serial.println("[INIT] ES8311 found at 0x18!");
    }
    
    // ---- Step 2: Enable PA ----
    Serial.println("[INIT] Step 2: PA Enable (GPIO46)");
    pa_enable(true);
    delay(50);
    
    // ---- Step 3: Initialize ES8311 ----
    Serial.println("[INIT] Step 3: ES8311 codec");
    if (!es8311_init_codec()) {
        Serial.println("[INIT] ERROR: ES8311 init failed! Dumping regs...");
        es8311_dump_regs();
        // Continue anyway
    }
    
    // Dump registers for verification
    Serial.println("[INIT] ES8311 register dump after init:");
    es8311_dump_regs();
    
    // ---- Step 4: Initialize I2S ----
    Serial.println("[INIT] Step 4: I2S driver");
    if (!i2s_init_driver()) {
        Serial.println("[INIT] ERROR: I2S init failed!");
        return;
    }
    
    // ---- Step 5: Play test tone ----
    Serial.println("[INIT] Step 5: Audio test (1kHz tone, 2 seconds)");
    delay(500);
    play_test_tone(2000);
    
    // ---- Step 6: Connect WiFi ----
    Serial.println("[INIT] Step 6: WiFi connect");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
        delay(500);
        Serial.print(".");
        wifi_attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[INIT] WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[INIT] WiFi connection FAILED!");
    }
    
    Serial.println("\n[INIT] Setup complete! Starting polling loop...\n");
}

// ============================================================
// LOOP
// ============================================================
unsigned long last_poll = 0;

void loop() {
    unsigned long now = millis();
    
    // Poll for medication reminders
    if (WiFi.status() == WL_CONNECTED && (now - last_poll > POLL_INTERVAL || last_poll == 0)) {
        poll_medication_reminders();
        last_poll = now;
    }
    
    delay(1000);
}
