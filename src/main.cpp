/*
 * ESP-BOX-3 Medication Reminder Firmware
 * 
 * Hardware: ESP32-S3-BOX-3 (ESP32-S3 + ES8311 DAC + ES7210 ADC)
 * Framework: Arduino (PlatformIO)
 * 
 * Audio Path TX: ESP32-S3 I2S0 -> ES8311 DAC -> PA (GPIO46) -> Speaker
 * Audio Path RX: ES7210 ADC -> ESP32-S3 I2S1 (slave, clock from I2S0)
 * 
 * Pin Definitions (from ESP-BOX-3 schematic & xiaozhi-esp32 BSP):
 *   I2C SDA  = GPIO8   (Wire / I2C_NUM_0, with internal pullups)
 *   I2C SCL  = GPIO18  (Wire / I2C_NUM_0, with internal pullups)
 *   I2S MCLK = GPIO2   (I2S0 master, drives both ES8311 and ES7210)
 *   I2S BCLK = GPIO17  (I2S0 master output, I2S1 slave input)
 *   I2S WS   = GPIO45  (I2S0 master output, I2S1 slave input)
 *   I2S DOUT = GPIO15  (to ES8311 DAC, I2S0 TX)
 *   I2S DIN  = GPIO16  (from ES7210 ADC, I2S1 RX)
 *   PA EN    = GPIO46  (HIGH = speaker enabled)
 *   Voice BTN= GPIO1   (top Mute button, active LOW)
 *   ES8311 I2C Address = 0x18
 *   ES7210 I2C Address = 0x40
 *
 * LCD Pin Definitions (from ESP-BOX-3 BSP, confirmed ILI9341 panel):
 *   SPI MOSI  = GPIO6  (SPI2_HOST, from official esp-brookesia BSP)
 *   SPI SCLK  = GPIO7
 *   SPI CS    = GPIO5
 *   SPI DC    = GPIO4
 *   RST       = GPIO48
 *   Backlight = GPIO47  (LEDC PWM)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <driver/spi_master.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_heap_caps.h"

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
#define VOICE_BTN       1       // Top Mute button (GPIO1, active LOW)
#define ES8311_ADDR     0x18   // 7-bit I2C address
#define ES7210_ADDR     0x40   // 7-bit I2C address

// ============================================================
// LCD PIN DEFINITIONS - ESP-BOX-3 (ILI9341 panel)
// ============================================================
#define LCD_BACKLIGHT   47      // Backlight PWM (LEDC)
#define LCD_CS          5       // SPI CS
#define LCD_DC          4       // SPI DC (Data/Command)
#define LCD_RST         48      // Reset pin
#define LCD_MOSI        6       // SPI MOSI
#define LCD_SCLK        7       // SPI SCLK

// ============================================================
// AUDIO CONFIG
// ============================================================
#define SAMPLE_RATE     16000
#define I2S_PORT_TX     I2S_NUM_0   // TX master (ES8311 speaker)
#define I2S_PORT_RX     I2S_NUM_1   // RX slave  (ES7210 microphone)
#define I2S_BITS        16

// ============================================================
// VOICE RECORDING CONFIG
// ============================================================
#define VAD_THRESHOLD   200       // VAD amplitude threshold
#define MAX_RECORD_MS   5000      // Max recording duration
#define RECORD_CHUNK    1024      // I2S read chunk size (bytes)

// ============================================================
// WIFI CONFIG - Change these!
// ============================================================
#define WIFI_SSID       "byj"
#define WIFI_PASS       "REDACTED_WIFI_PASSWORD"

// ============================================================
// SERVER CONFIG
// ============================================================
#define SERVER_BASE     "http://YOUR_SERVER_IP:3000"
#define DEVICE_MAC     "e8:f6:0a:a8:c3:bc"
#define POLL_INTERVAL   60000   // 60s polling interval

// ============================================================
// VOLUME (0-100)
// ============================================================
#define VOLUME          95

// ============================================================
// ES8311 Register Write Helper (with 3x retry)
// ============================================================
static bool es8311_write_reg(uint8_t reg, uint8_t val) {
    for (int attempt = 0; attempt < 3; attempt++) {
        Wire.beginTransmission(ES8311_ADDR);
        Wire.write(reg);
        Wire.write(val);
        if (Wire.endTransmission() == 0) return true;
        delay(5);
    }
    return false;
}

static int es8311_read_reg(uint8_t reg) {
    for (int attempt = 0; attempt < 3; attempt++) {
        Wire.beginTransmission(ES8311_ADDR);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) { delay(5); continue; }
        Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
        if (Wire.available()) return Wire.read();
        delay(5);
    }
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
// ES7210 Register Definitions (from esp-bsp/es7210_reg.h)
// ============================================================
#define ES7210_RESET_REG00              0x00
#define ES7210_CLOCK_OFF_REG01          0x01
#define ES7210_DOUBLE_REG02             0x02
#define ES7210_SR_REG03                 0x03
#define ES7210_MODE_CONFIG_REG08        0x08
#define ES7210_ADC_OSR_REG04            0x04
#define ES7210_ADC1_OFFSET_REG06        0x06
#define ES7210_ADC2_OFFSET_REG07        0x07
#define ES7210_POWER_DOWN_REG06         0x06
#define ES7210_REG09                    0x09
#define ES7210_REG0A                    0x0A
#define ES7210_ADC12_HPF1_REG23         0x23
#define ES7210_ADC12_HPF2_REG22         0x22
#define ES7210_ADC34_HPF1_REG21         0x21
#define ES7210_ADC34_HPF2_REG20         0x20
#define ES7210_MIC12_POWER_REG4B        0x4B
#define ES7210_MIC34_POWER_REG4C        0x4C
#define ES7210_MIC1_GAIN_REG44          0x44
#define ES7210_MIC2_GAIN_REG45          0x45
#define ES7210_MIC3_GAIN_REG46          0x46
#define ES7210_MIC4_GAIN_REG47          0x47
#define ES7210_MAINCLK_REG0D            0x0D
#define ES7210_CLK_DET_REG0E            0x0E
#define ES7210_SDP_INTERFACE1_REG11     0x11
#define ES7210_SDP_INTERFACE2_REG12     0x12
#define ES7210_ANALOG_REG40             0x40
#define ES7210_MICBIAS_REG41            0x41

// ES7210 I2C helpers
static bool es7210_write_reg(uint8_t reg, uint8_t val) {
    for (int attempt = 0; attempt < 3; attempt++) {
        Wire.beginTransmission(ES7210_ADDR);
        Wire.write(reg);
        Wire.write(val);
        if (Wire.endTransmission() == 0) return true;
        delay(5);
    }
    return false;
}

static int es7210_read_reg(uint8_t reg) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    Wire.requestFrom((uint8_t)ES7210_ADDR, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return -1;
}

// ============================================================
// ES7210 Full Initialization (based on ESPHome + esp-bsp driver)
// KEY FIXES from v5.x-v6.x debugging:
//   - CLOCK_OFF_REG01 = 0x3F (safe static state during config)
//   - POWER_DOWN_REG06 = 0x04 (DLL bypass power-down, per ESPHome)
//   - SDP_INTERFACE2_REG12 = 0x00 (standard I2S, NOT TDM)
//   - MODE_CONFIG_REG08 = 0x10 (2ch standard slave)
// ============================================================
static bool es7210_init_codec() {
    Serial.println("[ES7210] Starting initialization...");
    bool ok = true;

    // Step 1: Reset
    ok &= es7210_write_reg(ES7210_RESET_REG00, 0xFF);
    delay(30);
    ok &= es7210_write_reg(ES7210_RESET_REG00, 0x32);
    delay(10);

    // Step 1b: Close all clocks during config (safe static state)
    ok &= es7210_write_reg(ES7210_CLOCK_OFF_REG01, 0x3F);

    // Step 2: Set initialization time
    ok &= es7210_write_reg(0x04, 0x01);
    ok &= es7210_write_reg(0x05, 0x00);
    ok &= es7210_write_reg(0x06, 0x01);
    ok &= es7210_write_reg(0x07, 0x00);

    // Step 3: HPF config
    ok &= es7210_write_reg(ES7210_ADC12_HPF2_REG22, 0x2A);
    ok &= es7210_write_reg(ES7210_ADC12_HPF1_REG23, 0x0A);
    ok &= es7210_write_reg(ES7210_ADC34_HPF2_REG20, 0x0A);
    ok &= es7210_write_reg(ES7210_ADC34_HPF1_REG21, 0x2A);

    // Step 4: I2S format - 16-bit standard stereo (NOT TDM!)
    ok &= es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, 0x60);  // 16-bit, ADC1/2 on SDOUT1
    ok &= es7210_write_reg(ES7210_SDP_INTERFACE2_REG12, 0x00);  // Standard I2S mode

    // Step 5: Mode config - 2ch standard, slave
    ok &= es7210_write_reg(ES7210_MODE_CONFIG_REG08, 0x10);

    // Step 6: Analog power
    ok &= es7210_write_reg(ES7210_ANALOG_REG40, 0x43);
    ok &= es7210_write_reg(ES7210_MICBIAS_REG41, 0x00);

    // Step 7: MIC bias and power
    ok &= es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x00);
    ok &= es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x00);

    // Step 8: MIC1 gain (0x00=0dB, 0x01=3dB, ... max 0x0A=30dB)
    ok &= es7210_write_reg(ES7210_MIC1_GAIN_REG44, 0x01);  // 3dB
    ok &= es7210_write_reg(ES7210_MIC2_GAIN_REG45, 0x01);

    // Step 9: Main clock - 0xC1 for 16kHz with MCLK=4.096MHz, DLL bypass
    ok &= es7210_write_reg(ES7210_MAINCLK_REG0D, 0xC1);
    ok &= es7210_write_reg(ES7210_CLK_DET_REG0E, 0x00);

    // Step 10: SR_REG03 - 16kHz
    ok &= es7210_write_reg(ES7210_SR_REG03, 0x00);

    // Step 11: Power down DLL (DLL_BYPASS=1 in MAINCLK, so power it down)
    // ESPHome writes 0x04 here. 0x00 would keep DLL powered but it's bypassed.
    ok &= es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x04);

    if (!ok) {
        Serial.println("[ES7210] ERROR: Some register writes failed!");
        return false;
    }

    // Verify key registers
    Serial.println("[ES7210] Register verification:");
    Serial.printf("  RESET(0x00)     = 0x%02X (expect 0x32)\n", es7210_read_reg(0x00));
    Serial.printf("  CLOCK_OFF(0x01) = 0x%02X (expect 0x3F)\n", es7210_read_reg(0x01));
    Serial.printf("  PWR_DN(0x06)    = 0x%02X (expect 0x04)\n", es7210_read_reg(0x06));
    Serial.printf("  MODE_CFG(0x08)  = 0x%02X (expect 0x10)\n", es7210_read_reg(0x08));
    Serial.printf("  SDP1(0x11)      = 0x%02X (expect 0x60)\n", es7210_read_reg(0x11));
    Serial.printf("  SDP2(0x12)      = 0x%02X (expect 0x00)\n", es7210_read_reg(0x12));
    Serial.printf("  MAINCLK(0x0D)   = 0x%02X (expect 0xC1)\n", es7210_read_reg(0x0D));

    Serial.println("[ES7210] Initialization complete!");
    return true;
}

// ============================================================
// I2S Dual-Port Initialization
// I2S_NUM_0 (TX Master): generates BCLK/WS/MCLK for ES8311 + ES7210
// I2S_NUM_1 (RX Slave):  reads ES7210 data using clock from I2S0
// ============================================================

// TX silence task - keeps I2S0 clock running continuously
static TaskHandle_t tx_silence_task_handle = NULL;
static volatile bool tx_silence_active = false;

static void tx_silence_task(void* param) {
    const size_t buf_bytes = 1024;  // 256 stereo frames * 4 bytes
    int16_t* silence_buf = (int16_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL);
    if (!silence_buf) {
        Serial.println("[TX-SILENCE] ERROR: alloc failed!");
        vTaskDelete(NULL);
        return;
    }
    memset(silence_buf, 0, buf_bytes);
    
    Serial.println("[TX-SILENCE] Task started, keeping I2S clock alive...");
    while (tx_silence_active) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_write(I2S_PORT_TX, silence_buf, buf_bytes,
                                   &bytes_written, pdMS_TO_TICKS(50));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            Serial.printf("[TX-SILENCE] i2s_write error: %s\n", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    free(silence_buf);
    Serial.println("[TX-SILENCE] Task stopped.");
    vTaskDelete(NULL);
}

static void tx_silence_pause() {
    tx_silence_active = false;
    vTaskDelay(pdMS_TO_TICKS(20));  // Let task exit write loop
}

static void tx_silence_resume() {
    if (!tx_silence_active) {
        tx_silence_active = true;
        xTaskCreatePinnedToCore(tx_silence_task, "tx_silence", 4096, NULL, 3, &tx_silence_task_handle, 0);
    }
}

static bool i2s_init_driver() {
    Serial.println("[I2S] Initializing dual-port (I2S0 TX master + I2S1 RX slave)...");
    
    // ---- I2S0: TX Master (ES8311 speaker) ----
    i2s_config_t i2s_tx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 4096000
    };
    
    i2s_pin_config_t tx_pin_config = {
        .mck_io_num = I2S_MCLK,
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = -1  // TX only
    };
    
    esp_err_t err;
    err = i2s_driver_install(I2S_PORT_TX, &i2s_tx_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] TX driver install failed: %s\n", esp_err_to_name(err));
        return false;
    }
    err = i2s_set_pin(I2S_PORT_TX, &tx_pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S] TX set pin failed: %s\n", esp_err_to_name(err));
        return false;
    }
    err = i2s_start(I2S_PORT_TX);
    if (err != ESP_OK) {
        Serial.printf("[I2S] TX start failed: %s\n", esp_err_to_name(err));
        return false;
    }
    Serial.println("[I2S] I2S0 (TX master) initialized and started!");
    
    // ---- I2S1: RX Slave (ES7210 microphone) ----
    i2s_config_t i2s_rx_config = {
        .mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,  // Only one port can use APLL
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0     // Slave doesn't generate MCLK
    };
    
    i2s_pin_config_t rx_pin_config = {
        .mck_io_num = -1,          // No MCLK (provided by I2S0)
        .bck_io_num = I2S_BCLK,    // Shared with I2S0 (input)
        .ws_io_num = I2S_WS,       // Shared with I2S0 (input)
        .data_out_num = -1,        // RX only
        .data_in_num = I2S_DIN     // ES7210 data
    };
    
    err = i2s_driver_install(I2S_PORT_RX, &i2s_rx_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] RX driver install failed: %s\n", esp_err_to_name(err));
        // Continue anyway - TX still works for speaker
    } else {
        err = i2s_set_pin(I2S_PORT_RX, &rx_pin_config);
        if (err != ESP_OK) {
            Serial.printf("[I2S] RX set pin failed: %s\n", esp_err_to_name(err));
        } else {
            err = i2s_start(I2S_PORT_RX);
            if (err != ESP_OK) {
                Serial.printf("[I2S] RX start failed: %s\n", esp_err_to_name(err));
            } else {
                Serial.println("[I2S] I2S1 (RX slave) initialized and started!");
            }
        }
    }
    
    // Start TX silence task to keep clock running
    tx_silence_resume();
    
    Serial.println("[I2S] Dual-port initialization complete!");
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
    
    tx_silence_pause();  // Stop silence to avoid interference
    pa_enable(true);
    
    const double freq = 1000.0;
    const int samples = (SAMPLE_RATE * duration_ms) / 1000;
    const int16_t amplitude = 16000;
    
    const int chunk_samples = 256;
    int16_t buf[chunk_samples * 2];
    int played = 0;
    
    while (played < samples) {
        int n = min(chunk_samples, samples - played);
        for (int i = 0; i < n; i++) {
            double t = (double)(played + i) / SAMPLE_RATE;
            int16_t val = (int16_t)(amplitude * sin(2.0 * PI * freq * t));
            buf[i * 2] = val;
            buf[i * 2 + 1] = val;
        }
        size_t bytes_written = 0;
        i2s_write(I2S_PORT_TX, buf, n * 4, &bytes_written, portMAX_DELAY);
        played += n;
    }
    
    pa_enable(false);       // Turn off PA to avoid feedback
    tx_silence_resume();    // Resume clock keep-alive
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
    tx_silence_pause();  // Stop silence during playback
    
    const uint8_t* audio_ptr = data + data_offset;
    size_t remaining = min(audio_data_len, len - data_offset);
    
    const int buf_samples = 512;
    int16_t out_buf[buf_samples * 2];
    size_t pos = 0;
    
    while (pos < remaining) {
        int samples_to_fill = buf_samples;
        if (channels == 1 && bits == 16) {
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
        i2s_write(I2S_PORT_TX, out_buf, samples_to_fill * 4, &bytes_written, portMAX_DELAY);
    }
    
    Serial.printf("[WAV] Playback complete, %zu bytes played\n", pos);
    pa_enable(false);
    tx_silence_resume();  // Resume clock keep-alive
}

// ============================================================
// PLAYED-ID DEDUP (anti-repeat)
// ============================================================
#define MAX_PLAYED_IDS  20
static String played_ids[MAX_PLAYED_IDS];
static int played_idx = 0;
static bool was_played(const String& id);
static void mark_as_played_local(const String& id);

// ============================================================
// LCD MODULE - Raw SPI ILI9341 Driver (no external libraries)
// ============================================================

// ILI9341 Commands
#define ILI9341_SWRESET  0x01
#define ILI9341_SLPOUT   0x11
#define ILI9341_NORON    0x13
#define ILI9341_INVOFF   0x20
#define ILI9341_DISPON   0x29
#define ILI9341_CASET    0x2A
#define ILI9341_PASET    0x2B
#define ILI9341_RAMWR    0x2C
#define ILI9341_MADCTL   0x36
#define ILI9341_PIXFMT   0x3A
#define ILI9341_FRMCTR1  0xB1
#define ILI9341_FRMCTR2  0xB2
#define ILI9341_FRMCTR3  0xB3
#define ILI9341_INVCTR   0xB4
#define ILI9341_PWCTR1   0xC0
#define ILI9341_PWCTR2   0xC1
#define ILI9341_PWCTR3   0xC2
#define ILI9341_PWCTR4   0xC3
#define ILI9341_PWCTR5   0xC4
#define ILI9341_VMCTR1   0xC5
#define ILI9341_GMCTRP1  0xE0
#define ILI9341_GMCTRN1  0xE1

// RGB565 Color constants
#define LCD_BLACK   0x0000
#define LCD_WHITE   0xFFFF
#define LCD_RED     0xF800
#define LCD_GREEN   0x07E0
#define LCD_BLUE    0x001F
#define LCD_CYAN    0x07FF
#define LCD_YELLOW  0xFFE0

// LCD state
static uint16_t _lcd_w = 240;  // Portrait width
static uint16_t _lcd_h = 320;  // Portrait height
static uint16_t _lcd_fg = LCD_WHITE;
static uint16_t _lcd_bg = LCD_BLACK;
static uint8_t  _lcd_textsize = 1;
static int16_t  _lcd_cursor_x = 0;
static int16_t  _lcd_cursor_y = 0;

// Forward declarations for LCD show functions
static void lcd_show_reminder(const char* med_name);

// ESP-IDF SPI master driver for LCD (more reliable than Arduino SPI on ESP32-S3)
static spi_device_handle_t lcd_spi;

// Pre-transfer callback: set D/C pin before each transaction
static IRAM_ATTR void lcd_spi_pre_cb(spi_transaction_t *t) {
    int dc = (int)((intptr_t)t->user & 0x01);
    gpio_set_level((gpio_num_t)LCD_DC, dc);
}

// ============================================================
// Bit-bang pre-init: REMOVED in v7.18 (was conflicting with BSP vendor init)
// ============================================================

// SPI low-level communication via ESP-IDF (40MHz, SPI3_HOST - matches official BSP)
static void lcd_spi_init() {
    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    digitalWrite(LCD_DC, HIGH);
    
    // SPI bus configuration (ESP-IDF style) - match official BSP: SPI3_HOST, 40MHz
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_MOSI,       // GPIO6
        .miso_io_num = -1,              // Not used
        .sclk_io_num = LCD_SCLK,       // GPIO7
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 6400,      // Max transfer: width * 2 * 10
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK,
    };
    
    // SPI device configuration - match official BSP settings
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,                         // SPI mode 0
        .duty_cycle_pos = 128,              // 50% duty cycle
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,              // Match BSP default (0)
        .clock_speed_hz = 40 * 1000 * 1000, // 40MHz (match official BSP)
        .input_delay_ns = 0,
        .spics_io_num = LCD_CS,            // GPIO5
        .flags = SPI_DEVICE_HALFDUPLEX,     // Half-duplex (TX only, match esp_lcd_panel_io_spi)
        .queue_size = 10,                   // Match BSP trans_queue_depth=10
        .pre_cb = lcd_spi_pre_cb,
        .post_cb = NULL,
    };
    
    esp_err_t err;
    err = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        Serial.printf("[LCD] SPI bus init failed: %s\n", esp_err_to_name(err));
        return;
    }
    
    err = spi_bus_add_device(SPI3_HOST, &dev_cfg, &lcd_spi);
    if (err != ESP_OK) {
        Serial.printf("[LCD] SPI device add failed: %s\n", esp_err_to_name(err));
        return;
    }
    
    Serial.println("[LCD] ESP-IDF SPI3_HOST bus init OK (40MHz)");
}

// DMA-safe static buffer for command/data writes (must NOT be on PSRAM stack!)
static uint8_t _lcd_dma_buf[4];
// Larger DMA buffer for init commands (gamma needs 15 bytes)
static uint8_t _lcd_init_buf[16];

// Write command (1 byte, D/C=LOW)
static void lcd_write_cmd(uint8_t cmd) {
    _lcd_dma_buf[0] = cmd;
    spi_transaction_t t = {};
    t.length = 8;             // 1 byte = 8 bits
    t.tx_buffer = _lcd_dma_buf;
    t.user = (void*)0;        // D/C = LOW (command)
    spi_device_transmit(lcd_spi, &t);
}

// Write data byte (1 byte, D/C=HIGH)
static void lcd_write_data8(uint8_t d) {
    _lcd_dma_buf[0] = d;
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = _lcd_dma_buf;
    t.user = (void*)1;        // D/C = HIGH (data)
    spi_device_transmit(lcd_spi, &t);
}

// Write multiple data bytes in single SPI transaction (D/C=HIGH, CS stays LOW)
static void lcd_write_data_buf(const uint8_t* data, int len) {
    if (len > 16) len = 16;  // Safety: max 16 bytes
    memcpy(_lcd_init_buf, data, len);
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = _lcd_init_buf;
    t.user = (void*)1;        // D/C = HIGH
    spi_device_transmit(lcd_spi, &t);
}

// Write data 16-bit (2 bytes, D/C=HIGH)
static void lcd_write_data16(uint16_t d) {
    _lcd_dma_buf[0] = (uint8_t)(d >> 8);
    _lcd_dma_buf[1] = (uint8_t)(d & 0xFF);
    spi_transaction_t t = {};
    t.length = 16;
    t.tx_buffer = _lcd_dma_buf;
    t.user = (void*)1;
    spi_device_transmit(lcd_spi, &t);
}

// Set address window for pixel writes
// CASET/PASET 4-byte params sent as single SPI transaction (CS stays LOW)
static void lcd_set_addr(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    // CASET command
    lcd_write_cmd(ILI9341_CASET);
    // CASET data: 4 bytes in one transaction
    _lcd_dma_buf[0] = (uint8_t)(x1 >> 8);
    _lcd_dma_buf[1] = (uint8_t)(x1 & 0xFF);
    _lcd_dma_buf[2] = (uint8_t)(x2 >> 8);
    _lcd_dma_buf[3] = (uint8_t)(x2 & 0xFF);
    {
        spi_transaction_t t = {};
        t.length = 32;  // 4 bytes = 32 bits
        t.tx_buffer = _lcd_dma_buf;
        t.user = (void*)1;  // D/C = HIGH
        spi_device_transmit(lcd_spi, &t);
    }
    // PASET command
    lcd_write_cmd(ILI9341_PASET);
    // PASET data: 4 bytes in one transaction
    _lcd_dma_buf[0] = (uint8_t)(y1 >> 8);
    _lcd_dma_buf[1] = (uint8_t)(y1 & 0xFF);
    _lcd_dma_buf[2] = (uint8_t)(y2 >> 8);
    _lcd_dma_buf[3] = (uint8_t)(y2 & 0xFF);
    {
        spi_transaction_t t = {};
        t.length = 32;
        t.tx_buffer = _lcd_dma_buf;
        t.user = (void*)1;
        spi_device_transmit(lcd_spi, &t);
    }
    // RAMWR command
    lcd_write_cmd(ILI9341_RAMWR);
}

// ILI9341 initialization sequence (official ESP-BOX-3 BSP vendor init)
// Source: https://github.com/espressif/esp-bsp/blob/master/bsp/esp-box-3/esp-box-3.c
static void lcd_init_panel() {
    // Hardware reset (reset_active_high=true from official BSP: HIGH=reset, LOW=run)
    digitalWrite(LCD_RST, HIGH);  // Enter reset
    delay(20);
    digitalWrite(LCD_RST, LOW);   // Release reset
    delay(150);

    // ---- Official BSP vendor init sequence ----
    // Vendor command (NVM unlock / panel-specific)
    lcd_write_cmd(0xC8);
    lcd_write_data_buf((const uint8_t[]){0xFF, 0x93, 0x42}, 3);

    // Power Control 1
    lcd_write_cmd(0xC0);
    lcd_write_data_buf((const uint8_t[]){0x0E, 0x0E}, 2);

    // VCOM Control 1
    lcd_write_cmd(0xC5);
    lcd_write_data_buf((const uint8_t[]){0xD0}, 1);

    // Power Control 2
    lcd_write_cmd(0xC1);
    lcd_write_data_buf((const uint8_t[]){0x02}, 1);

    // Display Inversion Control
    lcd_write_cmd(0xB4);
    lcd_write_data_buf((const uint8_t[]){0x02}, 1);

    // Positive Gamma Correction
    lcd_write_cmd(0xE0);
    lcd_write_data_buf((const uint8_t[]){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39,
                                         0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15);

    // Negative Gamma Correction
    lcd_write_cmd(0xE1);
    lcd_write_data_buf((const uint8_t[]){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33,
                                         0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15);

    // Frame Rate Control
    lcd_write_cmd(0xB1);
    lcd_write_data_buf((const uint8_t[]){0x00, 0x1B}, 2);

    // MADCTL: portrait, mirror X+Y, BGR
    // BSP uses 0x08 then esp_lcd_panel_mirror(true,true) adds MX+MY → 0xC8
    // We set 0xC8 directly for portrait 240x320
    lcd_write_cmd(ILI9341_MADCTL);
    lcd_write_data8(0xC8);  // MY=1,MX=1,MV=0,BGR=1 (portrait 240x320, image upright)

    // Pixel format: 16-bit/pixel (RGB565)
    lcd_write_cmd(ILI9341_PIXFMT);
    lcd_write_data8(0x55);

    // Entry Mode Set (BSP includes this, standard Adafruit does not)
    lcd_write_cmd(0xB7);
    lcd_write_data8(0x06);

    // Sleep Out
    lcd_write_cmd(ILI9341_SLPOUT);
    delay(128);  // BSP uses 0x80 flag = 128ms delay

    // Display ON
    lcd_write_cmd(ILI9341_DISPON);
    delay(128);  // BSP uses 0x80 flag = 128ms delay
}

// Fill screen with solid color (using DMA for speed)
// Writes 244 columns (4 extra beyond 240) to cover grey bar on right edge
static void lcd_fill_screen(uint16_t color) {
    const int FILL_COLS = 244;  // 240 visible + 4 extra to cover grey bar
    lcd_set_addr(0, 0, FILL_COLS - 1, _lcd_h - 1);
    
    // Build one line of pixel data (DMA-capable static buffer)
    static uint8_t line_buf[256 * 2];
    for (int i = 0; i < FILL_COLS; i++) {
        line_buf[i * 2] = (uint8_t)(color >> 8);
        line_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }
    
    // Send one line at a time via DMA
    for (int row = 0; row < _lcd_h; row++) {
        spi_transaction_t t = {};
        t.length = FILL_COLS * 2 * 8;    // bits
        t.tx_buffer = line_buf;
        t.user = (void*)1;             // D/C = HIGH
        spi_device_transmit(lcd_spi, &t);
    }
}

// Fill rectangle (using DMA for speed)
static void lcd_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x >= _lcd_w || y >= _lcd_h) return;
    if (x + w > _lcd_w) w = _lcd_w - x;
    if (y + h > _lcd_h) h = _lcd_h - y;
    if (w <= 0 || h <= 0) return;
    
    lcd_set_addr(x, y, x + w - 1, y + h - 1);
    
    // Build one row of pixel data
    static uint8_t row_buf[320 * 2];
    for (int i = 0; i < w; i++) {
        row_buf[i * 2] = (uint8_t)(color >> 8);
        row_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }
    
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = {};
        t.length = w * 2 * 8;
        t.tx_buffer = row_buf;
        t.user = (void*)1;
        spi_device_transmit(lcd_spi, &t);
    }
}

// Read ILI9341 display ID (debug)
static uint32_t lcd_read_id() {
    uint8_t rx_buf[4] = {0};
    // Send RDDID command (0x04)
    uint8_t cmd = 0x04;
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = (void*)0;  // D/C = LOW (command)
    spi_device_transmit(lcd_spi, &t);
    
    // Read 4 bytes (dummy + 3 ID bytes)
    t.length = 32;
    t.rx_buffer = rx_buf;
    t.tx_buffer = NULL;
    t.user = (void*)1;   // D/C = HIGH (data)
    spi_device_transmit(lcd_spi, &t);
    
    return ((uint32_t)rx_buf[0] << 16) | ((uint32_t)rx_buf[1] << 8) | rx_buf[2];
}

// ============================================================
// 5x7 Bitmap Font (standard GLCD font, ASCII 0x20-0x7E)
// Each char: 5 bytes (columns), 7 rows per column (bit 0=top)
// ============================================================
static const uint8_t font5x7[] PROGMEM = {
    0x00,0x00,0x00,0x00,0x00, // (space)
    0x00,0x00,0x5F,0x00,0x00, // !
    0x00,0x07,0x00,0x07,0x00, // "
    0x14,0x7F,0x14,0x7F,0x14, // #
    0x24,0x2A,0x7F,0x2A,0x12, // $
    0x23,0x13,0x08,0x64,0x62, // %
    0x36,0x49,0x55,0x22,0x50, // &
    0x00,0x05,0x03,0x00,0x00, // '
    0x00,0x1C,0x22,0x41,0x00, // (
    0x00,0x41,0x22,0x1C,0x00, // )
    0x14,0x08,0x3E,0x08,0x14, // *
    0x08,0x08,0x3E,0x08,0x08, // +
    0x00,0x50,0x30,0x00,0x00, // ,
    0x08,0x08,0x08,0x08,0x08, // -
    0x00,0x60,0x60,0x00,0x00, // .
    0x20,0x10,0x08,0x04,0x02, // /
    0x3E,0x51,0x49,0x45,0x3E, // 0
    0x00,0x42,0x7F,0x40,0x00, // 1
    0x42,0x61,0x51,0x49,0x46, // 2
    0x21,0x41,0x45,0x4B,0x31, // 3
    0x18,0x14,0x12,0x7F,0x10, // 4
    0x27,0x45,0x45,0x45,0x39, // 5
    0x3C,0x4A,0x49,0x49,0x30, // 6
    0x01,0x71,0x09,0x05,0x03, // 7
    0x36,0x49,0x49,0x49,0x36, // 8
    0x06,0x49,0x49,0x29,0x1E, // 9
    0x00,0x36,0x36,0x00,0x00, // :
    0x00,0x56,0x36,0x00,0x00, // ;
    0x08,0x14,0x22,0x41,0x00, // <
    0x14,0x14,0x14,0x14,0x14, // =
    0x00,0x41,0x22,0x14,0x08, // >
    0x02,0x01,0x51,0x09,0x06, // ?
    0x32,0x49,0x79,0x41,0x3E, // @
    0x7E,0x11,0x11,0x11,0x7E, // A
    0x7F,0x49,0x49,0x49,0x36, // B
    0x3E,0x41,0x41,0x41,0x22, // C
    0x7F,0x41,0x41,0x22,0x1C, // D
    0x7F,0x49,0x49,0x49,0x41, // E
    0x7F,0x09,0x09,0x09,0x01, // F
    0x3E,0x41,0x49,0x49,0x7A, // G
    0x7F,0x08,0x08,0x08,0x7F, // H
    0x00,0x41,0x7F,0x41,0x00, // I
    0x20,0x40,0x41,0x3F,0x01, // J
    0x7F,0x08,0x14,0x22,0x41, // K
    0x7F,0x40,0x40,0x40,0x40, // L
    0x7F,0x02,0x0C,0x02,0x7F, // M
    0x7F,0x04,0x08,0x10,0x7F, // N
    0x3E,0x41,0x41,0x41,0x3E, // O
    0x7F,0x09,0x09,0x09,0x06, // P
    0x3E,0x41,0x51,0x21,0x5E, // Q
    0x7F,0x09,0x19,0x29,0x46, // R
    0x46,0x49,0x49,0x49,0x31, // S
    0x01,0x01,0x7F,0x01,0x01, // T
    0x3F,0x40,0x40,0x40,0x3F, // U
    0x1F,0x20,0x40,0x20,0x1F, // V
    0x3F,0x40,0x38,0x40,0x3F, // W
    0x63,0x14,0x08,0x14,0x63, // X
    0x07,0x08,0x70,0x08,0x07, // Y
    0x61,0x51,0x49,0x45,0x43, // Z
    0x00,0x7F,0x41,0x41,0x00, // [
    0x02,0x04,0x08,0x10,0x20, // backslash
    0x00,0x41,0x41,0x7F,0x00, // ]
    0x04,0x02,0x01,0x02,0x04, // ^
    0x40,0x40,0x40,0x40,0x40, // _
    0x00,0x03,0x05,0x00,0x00, // `
    0x20,0x54,0x54,0x54,0x78, // a
    0x7F,0x48,0x44,0x44,0x38, // b
    0x38,0x44,0x44,0x44,0x20, // c
    0x38,0x44,0x44,0x48,0x7F, // d
    0x38,0x54,0x54,0x54,0x18, // e
    0x08,0x7E,0x09,0x01,0x02, // f
    0x0C,0x52,0x52,0x52,0x3E, // g
    0x7F,0x08,0x04,0x04,0x78, // h
    0x00,0x44,0x7D,0x40,0x00, // i
    0x20,0x40,0x44,0x3D,0x00, // j
    0x7F,0x10,0x28,0x44,0x00, // k
    0x00,0x41,0x7F,0x40,0x00, // l
    0x7C,0x04,0x18,0x04,0x78, // m
    0x7C,0x08,0x04,0x04,0x78, // n
    0x38,0x44,0x44,0x44,0x38, // o
    0x7C,0x14,0x14,0x14,0x08, // p
    0x08,0x14,0x14,0x18,0x7C, // q
    0x7C,0x08,0x04,0x04,0x08, // r
    0x48,0x54,0x54,0x54,0x20, // s
    0x04,0x3F,0x44,0x40,0x20, // t
    0x3C,0x40,0x40,0x20,0x7C, // u
    0x1C,0x20,0x40,0x20,0x1C, // v
    0x3C,0x40,0x30,0x40,0x3C, // w
    0x44,0x28,0x10,0x28,0x44, // x
    0x0C,0x50,0x50,0x50,0x3C, // y
    0x44,0x64,0x54,0x4C,0x44, // z
    0x00,0x08,0x36,0x41,0x00, // {
    0x00,0x00,0x7F,0x00,0x00, // |
    0x00,0x41,0x36,0x08,0x00, // }
    0x10,0x08,0x08,0x10,0x08, // ~
};

// Draw a single character at (x, y) with text size
// Returns width of drawn character (in pixels, before scaling)
static int lcd_draw_char(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg, uint8_t size) {
    if (c < 0x20 || c > 0x7E) c = ' ';  // Replace non-printable with space
    
    const uint8_t *glyph = &font5x7[(c - 0x20) * 5];
    
    // For text size 1: char is 5 wide, 7 tall, 1px spacing
    // For text size N: char is 5*N wide, 7*N tall, N px spacing
    uint8_t col_w = 5 * size;
    uint8_t row_h = 7 * size;
    
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t line = pgm_read_byte(&glyph[col]);
        for (uint8_t row = 0; row < 7; row++) {
            uint16_t pix_color = (line & (1 << row)) ? color : bg;
            if (size == 1) {
                lcd_fill_rect(x + col, y + row, 1, 1, pix_color);
            } else {
                lcd_fill_rect(x + col * size, y + row * size, size, size, pix_color);
            }
        }
    }
    
    return col_w;  // Width of drawn character
}

// Print string at cursor position, update cursor
static void lcd_print(const char* str) {
    if (!str) return;
    while (*str) {
        char c = *str++;
        if (c == '\n') {
            _lcd_cursor_x = 0;
            _lcd_cursor_y += 7 * _lcd_textsize + 1;
        } else {
            int char_w = lcd_draw_char(_lcd_cursor_x, _lcd_cursor_y, c, _lcd_fg, _lcd_bg, _lcd_textsize);
            _lcd_cursor_x += char_w + _lcd_textsize;  // Add 1px spacing * size
            // Auto-wrap
            if (_lcd_cursor_x >= _lcd_w) {
                _lcd_cursor_x = 0;
                _lcd_cursor_y += 7 * _lcd_textsize + 1;
            }
        }
    }
}

// Print formatted string at cursor position
static void lcd_printf(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lcd_print(buf);
}

// Draw a diagnostic test pattern (color bars)
// Call after lcd_init_panel() to verify SPI writes reach the panel
static void lcd_test_pattern() {
    // Draw 8 color bars, each 40px wide (320/8=40)
    uint16_t colors[8] = {
        LCD_BLACK, LCD_BLUE, LCD_GREEN, LCD_CYAN,
        LCD_RED, LCD_YELLOW, LCD_WHITE, 0xFC00 // Orange-ish
    };
    for (int bar = 0; bar < 8; bar++) {
        uint16_t x = bar * 40;
        lcd_fill_rect(x, 0, 40, _lcd_h, colors[bar]);
    }
    Serial.println("[LCD] Test pattern drawn (8 color bars)");
}

// LCD text API (matching Adafruit style for easy code migration)
static inline void lcd_set_text_size(uint8_t s) { _lcd_textsize = s; }
static inline void lcd_set_text_color(uint16_t fg, uint16_t bg) { _lcd_fg = fg; _lcd_bg = bg; }
static inline void lcd_set_cursor(int16_t x, int16_t y) { _lcd_cursor_x = x; _lcd_cursor_y = y; }

// ============================================================
// LCD High-Level Functions (matching the original API)
// ============================================================

static void lcd_backlight_init() {
    ledcSetup(1, 5000, 10);       // Channel 1, 5kHz, 10-bit resolution
    ledcAttachPin(LCD_BACKLIGHT, 1);
    ledcWrite(1, 1023);           // Full brightness (100%)
    Serial.println("[LCD] Backlight initialized (100%)");
}

static void lcd_init() {
    Serial.println("[LCD] Initializing ILI9341 (BSP vendor init, SPI3_HOST 40MHz)...");

    // Step 1: Backlight ON first
    ledcSetup(1, 5000, 10);
    ledcAttachPin(LCD_BACKLIGHT, 1);
    ledcWrite(1, 1023);
    Serial.println("[LCD] Backlight ON");

    // Step 2: Configure RST/CS/DC pins (MOSI/SCLK configured by SPI driver)
    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    digitalWrite(LCD_DC, HIGH);
    digitalWrite(LCD_RST, LOW);  // Release reset (active HIGH)
    delay(10);

    // Step 3: SPI bus (SPI3_HOST, 40MHz - matches official BSP)
    Serial.println("[LCD] Init SPI bus (SPI3_HOST, 40MHz)...");
    lcd_spi_init();
    delay(50);
    Serial.println("[LCD] SPI bus OK");

    // Step 4: ILI9341 panel init (official BSP vendor sequence)
    Serial.println("[LCD] Init ILI9341 panel (BSP vendor init)...");
    lcd_init_panel();

    Serial.println("[LCD] ILI9341 initialized! (v7.22 flicker-free clock)");
}

static void lcd_show_boot_screen() {
    lcd_fill_screen(LCD_BLACK);
    lcd_set_text_color(LCD_WHITE, LCD_BLACK);
    lcd_set_text_size(3);
    lcd_set_cursor(39, 20);   // "ESP-BOX-3": 9 chars * 18px = 162, center: (240-162)/2 = 39
    lcd_print("ESP-BOX-3");
    lcd_set_text_size(2);
    lcd_set_cursor(6, 70);    // "Medication Reminder": 19 chars * 12px = 228, center: (240-228)/2 = 6
    lcd_print("Medication Reminder");
    lcd_set_text_size(1);
    lcd_set_text_color(LCD_CYAN, LCD_BLACK);
    lcd_set_cursor(75, 120);   // "Firmware v7.21": 15 chars * 6px = 90, center: (240-90)/2 = 75
    lcd_print("Firmware v7.22");
    lcd_set_text_color(LCD_YELLOW, LCD_BLACK);
    lcd_set_cursor(75, 150);   // "Initializing...": 15 chars * 6px = 90, center: (240-90)/2 = 75
    lcd_print("Initializing...");
}

static void lcd_show_wifi_connecting() {
    lcd_set_text_size(1);
    lcd_set_text_color(LCD_YELLOW, LCD_BLACK);
    lcd_fill_rect(80, 140, 180, 16, LCD_BLACK);
    lcd_set_cursor(80, 140);
    lcd_print("Connecting WiFi...");
}

static void lcd_show_wifi_connected(const char* ip) {
    lcd_set_text_size(1);
    lcd_set_text_color(LCD_GREEN, LCD_BLACK);
    lcd_fill_rect(80, 140, 200, 32, LCD_BLACK);
    lcd_set_cursor(80, 140);
    lcd_print("WiFi Connected!");
    lcd_set_cursor(80, 155);
    lcd_printf("IP: %s", ip);
}

static void lcd_show_status() {
    lcd_set_text_size(1);

    // WiFi status (y=140-155)
    lcd_set_text_color(LCD_GREEN, LCD_BLACK);
    lcd_fill_rect(0, 140, 240, 16, LCD_BLACK);
    if (WiFi.status() == WL_CONNECTED) {
        lcd_set_cursor(60, 140);
        lcd_printf("WiFi: %s", WiFi.localIP().toString().c_str());
    } else {
        lcd_set_cursor(60, 140);
        lcd_print("WiFi: Disconnected");
    }

    // System status (y=170-185)
    lcd_fill_rect(0, 170, 240, 32, LCD_BLACK);
    lcd_set_cursor(80, 170);
    lcd_print("System Ready");
    lcd_set_cursor(75, 185);
    lcd_print("Polling reminders...");
}

// ============================================================
// Flicker-free text rendering via in-memory framebuffer
// Builds the entire text image in RAM first, then blits to LCD
// in one shot — eliminates the "clear then draw" flicker that
// occurs when using lcd_fill_rect + lcd_print for frequently
// updated text (e.g. the clock display).
// ============================================================
static void lcd_draw_text_fb(int16_t x, int16_t y, const char* str,
                              uint8_t size, uint16_t fg, uint16_t bg) {
    int len = strlen(str);
    if (len == 0) return;

    int char_w  = 5 * size + size;   // 5 glyph columns * size + 1*size spacing
    int char_h  = 7 * size;          // 7 glyph rows * size
    int fb_w    = len * char_w;
    int fb_h    = char_h + 2;        // small vertical padding
    if (fb_w > _lcd_w) fb_w = _lcd_w;

    // Static framebuffer in internal SRAM (fast access, no alloc overhead)
    // Max usage: size=4, 8 chars → 192 wide × 30 tall = 5760 entries
    static uint16_t fb[200 * 40];       // 16 KB worst case
    static uint8_t  row_bytes[200 * 2]; // 400 bytes

    // Fill framebuffer with background color
    for (int i = 0; i < fb_w * fb_h; i++) fb[i] = bg;

    // Render characters into framebuffer
    int cx = 0;
    for (const char* p = str; *p && cx < fb_w; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E) c = ' ';
        const uint8_t *glyph = &font5x7[(c - 0x20) * 5];

        for (int col = 0; col < 5; col++) {
            uint8_t line = pgm_read_byte(&glyph[col]);
            for (int row = 0; row < 7; row++) {
                uint16_t pix = (line & (1 << row)) ? fg : bg;
                for (int dy = 0; dy < size; dy++) {
                    for (int dx = 0; dx < size; dx++) {
                        int px = cx + col * size + dx;
                        int py = row * size + dy;
                        if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                            fb[py * fb_w + px] = pix;
                    }
                }
            }
        }
        cx += char_w;
    }

    // Blit framebuffer to LCD row-by-row (each row = single SPI transaction)
    lcd_set_addr(x, y, x + fb_w - 1, y + fb_h - 1);
    for (int row = 0; row < fb_h; row++) {
        for (int col = 0; col < fb_w; col++) {
            uint16_t c = fb[row * fb_w + col];
            row_bytes[col * 2]     = (uint8_t)(c >> 8);
            row_bytes[col * 2 + 1] = (uint8_t)(c & 0xFF);
        }
        spi_transaction_t t = {};
        t.length    = fb_w * 2 * 8;
        t.tx_buffer = row_bytes;
        t.user      = (void*)1;   // D/C = HIGH
        spi_device_transmit(lcd_spi, &t);
    }
}

static void lcd_show_time() {
    struct tm timeinfo;
    bool has_time = getLocalTime(&timeinfo);

    char time_str[16];
    char date_str[16];
    if (has_time) {
        strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeinfo);
    } else {
        strcpy(time_str, "--:--:--");
        strcpy(date_str, "----  -  --");
    }

    // Time: size 4, centered — flicker-free framebuffer rendering
    // 8 chars * 24px = 192 → x = (240-192)/2 = 24
    lcd_draw_text_fb(24, 50, time_str, 4, LCD_WHITE, LCD_BLACK);

    // Date: size 2, centered — flicker-free framebuffer rendering
    // 10 chars * 12px = 120 → x = (240-120)/2 = 60
    lcd_draw_text_fb(60, 100, date_str, 2, LCD_CYAN, LCD_BLACK);
}

static void lcd_show_reminder(const char* med_name) {
    lcd_fill_screen(LCD_BLACK);
    lcd_set_text_size(2);
    lcd_set_text_color(LCD_RED, LCD_BLACK);
    lcd_set_cursor(24, 30);  // "!! MEDICATION !!": 16 chars * 12px = 192, center: (240-192)/2=24
    lcd_print("!! MEDICATION !!");

    lcd_set_text_size(3);
    lcd_set_text_color(LCD_YELLOW, LCD_BLACK);
    lcd_set_cursor(20, 80);   // med name: approx center, max ~10 chars * 18px = 180px
    char buf[22];
    strncpy(buf, med_name, 21);
    buf[21] = '\0';
    lcd_print(buf);

    lcd_set_text_size(2);
    lcd_set_text_color(LCD_WHITE, LCD_BLACK);
    lcd_set_cursor(66, 130);  // "TAKE NOW!": 9 chars * 12px = 108, center: (240-108)/2=66
    lcd_print("TAKE NOW!");

    lcd_set_text_size(1);
    lcd_set_text_color(LCD_GREEN, LCD_BLACK);
    lcd_set_cursor(66, 170);  // "Playing reminder...": 18 chars * 6px = 108, center: (240-108)/2=66
    lcd_print("Playing reminder...");
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
            String id = reminder["id"].as<String>();
            
            if (!played && strlen(audio_url) > 0 && !was_played(id)) {
                Serial.printf("[POLL] Playing reminder for: %s\n", med_name);
                lcd_show_reminder(med_name);
                download_and_play_wav(String(audio_url));
                mark_as_played_local(id);
                
                // Mark as played on server
                String mark_url = String(SERVER_BASE) + "/api/schedules/" +
                                  id + "/played";
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

static bool was_played(const String& id) {
    for (int i = 0; i < MAX_PLAYED_IDS; i++) {
        if (played_ids[i] == id) return true;
    }
    return false;
}

static void mark_as_played_local(const String& id) {
    played_ids[played_idx] = id;
    played_idx = (played_idx + 1) % MAX_PLAYED_IDS;
}

// ============================================================
// VOICE RECORDING (ES7210 → I2S1 RX → PSRAM → WAV upload)
// ============================================================
static volatile bool voice_recording = false;

// Simple WAV header writer
static void write_wav_header(uint8_t* header, uint32_t data_len, uint16_t channels, uint32_t sample_rate, uint16_t bits) {
    uint32_t byte_rate = sample_rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;
    uint32_t chunk_size = 36 + data_len;
    
    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &chunk_size, 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(header + 16, &fmt_size, 4);
    uint16_t audio_fmt = 1;
    memcpy(header + 20, &audio_fmt, 2);
    memcpy(header + 22, &channels, 2);
    memcpy(header + 24, &sample_rate, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    memcpy(header + 34, &bits, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &data_len, 4);
}

// Record audio from ES7210 via I2S1, then upload to server
static void record_and_upload_voice() {
    if (voice_recording) return;
    voice_recording = true;
    
    Serial.println("[VOICE] Starting recording...");
    
    // Allocate PSRAM buffer for raw stereo data
    // Max: 5s * 16kHz * 2ch * 2bytes = 320000 bytes
    size_t max_bytes = SAMPLE_RATE * 2 * 2 * (MAX_RECORD_MS / 1000);
    int16_t* raw_buf = (int16_t*)heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM);
    if (!raw_buf) {
        Serial.println("[VOICE] ERROR: PSRAM alloc failed!");
        voice_recording = false;
        return;
    }
    
    // Read I2S data with VAD
    size_t total_samples = 0;  // Mono samples
    size_t total_raw_bytes = 0;
    bool voice_detected = false;
    unsigned long start_ms = millis();
    unsigned long last_voice_ms = 0;
    
    uint8_t chunk[RECORD_CHUNK];
    
    // ---- Diagnostic: probe BCLK/WS before recording ----
    {
        pinMode(I2S_BCLK, INPUT_PULLUP);
        pinMode(I2S_WS, INPUT_PULLUP);
        int bclk_trans = 0, ws_trans = 0;
        bool bclk_last = digitalRead(I2S_BCLK);
        bool ws_last = digitalRead(I2S_WS);
        for (int i = 0; i < 5000; i++) {  // ~50ms
            bool b = digitalRead(I2S_BCLK);
            if (b != bclk_last) { bclk_trans++; bclk_last = b; }
            bool w = digitalRead(I2S_WS);
            if (w != ws_last) { ws_trans++; ws_last = w; }
            delayMicroseconds(10);
        }
        Serial.printf("[DIAG] BCLK(GPIO17) transitions in 50ms: %d (>1000=active)\n", bclk_trans);
        Serial.printf("[DIAG] WS(GPIO45) transitions in 50ms: %d (>50=active)\n", ws_trans);
        // Restore pin config for I2S
        // The I2S driver should re-claim these pins, but let's be safe
    }
    
    while (total_raw_bytes < max_bytes && 
           (millis() - start_ms < MAX_RECORD_MS)) {
        
        size_t bytes_read = 0;
        // Pre-fill with 0xAA for diagnostic
        memset(chunk, 0xAA, RECORD_CHUNK);
        esp_err_t err = i2s_read(I2S_PORT_RX, chunk, RECORD_CHUNK, &bytes_read, pdMS_TO_TICKS(100));
        
        if (err != ESP_OK || bytes_read == 0) {
            Serial.printf("[VOICE] i2s_read err=%s bytes=%d\n", esp_err_to_name(err), (int)bytes_read);
            continue;
        }
        
        // Convert stereo to mono and check VAD
        int16_t* samples = (int16_t*)chunk;
        int num_frames = bytes_read / 4;  // 2ch * 2bytes per frame
        
        for (int i = 0; i < num_frames && total_samples < max_bytes / 2; i++) {
            int16_t left = samples[i * 2];
            // int16_t right = samples[i * 2 + 1];
            int16_t mono = left;  // Use left channel (MIC1)
            
            raw_buf[total_samples++] = mono;
            total_raw_bytes += 2;
            
            // VAD check
            int16_t abs_val = (mono >= 0) ? mono : (int16_t)(-mono);
            if (abs_val > VAD_THRESHOLD) {
                voice_detected = true;
                last_voice_ms = millis();
            }
        }
        
        // Stop if silence after voice detected
        if (voice_detected && (millis() - last_voice_ms > 1000)) {
            Serial.println("[VOICE] Silence detected after voice, stopping.");
            break;
        }
    }
    
    // ---- Diagnostic: first samples stats ----
    if (total_samples > 0) {
        int16_t min_val = 32767, max_val = -32768;
        for (int i = 0; i < min((int)total_samples, 256); i++) {
            if (raw_buf[i] < min_val) min_val = raw_buf[i];
            if (raw_buf[i] > max_val) max_val = raw_buf[i];
        }
        Serial.printf("[DIAG] I2S-RX-chunk0: first 8 samples: ");
        for (int i = 0; i < 8 && i < (int)total_samples; i++) {
            Serial.printf("%d ", raw_buf[i]);
        }
        Serial.println();
        Serial.printf("[DIAG] I2S-RX-chunk0: min=%d max=%d range=%d\n", 
                      min_val, max_val, max_val - min_val);
    }
    
    Serial.printf("[VOICE] Recorded %d mono samples (%d ms), voice=%s\n",
                  (int)total_samples, (int)(total_samples * 1000 / SAMPLE_RATE),
                  voice_detected ? "YES" : "NO");
    
    // Fallback: if no voice detected, still send (server has 300ms fallback)
    if (total_samples < SAMPLE_RATE / 10) {  // Less than 100ms
        Serial.println("[VOICE] Too short, sending 3s forced recording");
        // Already have the data, just use it
    }
    
    if (total_samples == 0) {
        Serial.println("[VOICE] ERROR: No data recorded!");
        free(raw_buf);
        voice_recording = false;
        return;
    }
    
    // Build WAV in PSRAM
    size_t wav_size = 44 + total_samples * 2;
    uint8_t* wav_buf = (uint8_t*)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
    if (!wav_buf) {
        Serial.println("[VOICE] ERROR: WAV buffer alloc failed!");
        free(raw_buf);
        voice_recording = false;
        return;
    }
    
    write_wav_header(wav_buf, total_samples * 2, 1, SAMPLE_RATE, 16);
    memcpy(wav_buf + 44, raw_buf, total_samples * 2);
    free(raw_buf);
    
    // Upload to server
    String url = String(SERVER_BASE) + "/api/voice";
    Serial.printf("[VOICE] Uploading %zu bytes to %s\n", wav_size, url.c_str());
    
    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(5000);
    if (http.begin(url)) {
        http.addHeader("Content-Type", "audio/wav");
        http.setAuthorization("admin", "admin");
        int code = http.POST(wav_buf, wav_size);
        Serial.printf("[VOICE] Upload response: %d\n", code);
        if (code > 0) {
            String response = http.getString();
            Serial.printf("[VOICE] Server response: %s\n", response.c_str());
            
            // Parse TTS URL and play response
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, response);
            if (!err && doc.containsKey("ttsUrl")) {
                const char* tts_url = doc["ttsUrl"];
                Serial.printf("[VOICE] Playing TTS: %s\n", tts_url);
                download_and_play_wav(String(tts_url));
            }
        }
        http.end();
    } else {
        Serial.println("[VOICE] HTTP begin failed!");
    }
    
    free(wav_buf);
    voice_recording = false;
    Serial.println("[VOICE] Done.");
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);  // Wait for serial
    
    Serial.println("\n\n====================================");
    Serial.println("  Medication Reminder - ESP-BOX-3");
    Serial.println("  Firmware v7.22 (flicker-free clock display)");
    Serial.println("====================================\n");

    // ---- Step 0: Initialize LCD ----
    Serial.println("[INIT] Step 0: LCD (ILI9341)");
    lcd_init();
    lcd_show_boot_screen();

    // ---- Step 1: Initialize I2C (Wire / I2C_NUM_0) ----
    Serial.println("[INIT] Step 1: I2C (Wire / I2C_NUM_0)");
    Wire.begin(I2C_SDA, I2C_SCL, 100000);
    // NOTE: Do NOT call pinMode() on I2C pins after Wire.begin()!
    // Wire.begin() already configures internal pullups.
    // pinMode() would reconfigure them as GPIO, breaking I2C.
    delay(100);
    
    // Quick scan of known device addresses only (0x18=ES8311, 0x40=ES7210, 0x68=IMU)
    Serial.println("[INIT] Checking known I2C devices...");
    bool found_es8311 = false;
    bool found_es7210 = false;
    int known_addrs[] = {ES8311_ADDR, ES7210_ADDR, 0x68};
    for (int i = 0; i < 3; i++) {
        int addr = known_addrs[i];
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            Serial.printf("  I2C device found at 0x%02X\n", addr);
            if (addr == ES8311_ADDR) found_es8311 = true;
            if (addr == ES7210_ADDR) found_es7210 = true;
        } else {
            Serial.printf("  No device at 0x%02X (err=%d)\n", addr, err);
        }
    }
    
    if (!found_es8311) Serial.println("[INIT] WARNING: ES8311 NOT FOUND at 0x18!");
    else Serial.println("[INIT] ES8311 found at 0x18!");
    if (!found_es7210) Serial.println("[INIT] WARNING: ES7210 NOT FOUND at 0x40!");
    else Serial.println("[INIT] ES7210 found at 0x40!");
    
    // ---- Step 2: Enable PA ----
    Serial.println("[INIT] Step 2: PA Enable (GPIO46)");
    pa_enable(true);
    delay(50);
    
    // ---- Step 3: Initialize ES8311 (speaker DAC) ----
    Serial.println("[INIT] Step 3: ES8311 codec");
    if (!es8311_init_codec()) {
        Serial.println("[INIT] ERROR: ES8311 init failed! Dumping regs...");
        es8311_dump_regs();
    }
    
    // ---- Step 3b: Initialize ES7210 (mic ADC) ----
    Serial.println("[INIT] Step 3b: ES7210 codec");
    if (found_es7210) {
        if (!es7210_init_codec()) {
            Serial.println("[INIT] ERROR: ES7210 init failed!");
        }
    } else {
        Serial.println("[INIT] ES7210 not found, skipping mic init");
    }
    
    // ---- Step 4: Initialize I2S (dual port) ----
    Serial.println("[INIT] Step 4: I2S dual-port driver");
    if (!i2s_init_driver()) {
        Serial.println("[INIT] ERROR: I2S init failed!");
        return;
    }
    
    // ---- Step 5: Configure GPIO1 voice button ----
    Serial.println("[INIT] Step 5: Voice button (GPIO1)");
    pinMode(VOICE_BTN, INPUT_PULLUP);
    Serial.println("[INIT] GPIO1 configured (active LOW, top Mute button)");
    
    // ---- Step 6: Brief audio self-test ----
    Serial.println("[INIT] Step 6: Quick speaker test");
    delay(300);
    play_test_tone(500);  // 0.5s beep
    
    // ---- Step 7: Connect WiFi ----
    Serial.println("[INIT] Step 7: WiFi connect");
    lcd_show_wifi_connecting();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
        delay(500);
        Serial.print(".");
        wifi_attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[INIT] WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
        lcd_show_wifi_connected(WiFi.localIP().toString().c_str());
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("[INIT] NTP time sync initiated (async)");
    } else {
        Serial.println("\n[INIT] WiFi connection FAILED!");
        lcd_set_text_size(1);
        lcd_set_text_color(LCD_RED, LCD_BLACK);
        lcd_fill_rect(80, 140, 180, 16, LCD_BLACK);
        lcd_set_cursor(80, 140);
        lcd_print("WiFi FAILED!");
    }

    // Clear boot screen before entering normal display mode
    lcd_fill_screen(LCD_BLACK);
    lcd_show_time();
    lcd_show_status();

    Serial.println("\n[INIT] Setup complete! Starting polling loop...\n");
    Serial.println("[INIT] Press GPIO1 (top Mute button) to record voice.");
}

// ============================================================
// LOOP
// ============================================================
unsigned long last_poll = 0;

void loop() {
    unsigned long now = millis();

    // ---- Check GPIO1 voice button (active LOW) ----
    static bool btn_was_pressed = false;
    bool btn_now = (digitalRead(VOICE_BTN) == LOW);
    if (btn_now && !btn_was_pressed && !voice_recording) {
        Serial.println("[BTN] GPIO1 pressed! Starting voice recording...");
        delay(50);  // Simple debounce
        record_and_upload_voice();
    }
    btn_was_pressed = btn_now;

    // Update LCD clock display every second
    static unsigned long last_lcd_update = 0;
    if (now - last_lcd_update > 1000) {
        lcd_show_time();
        last_lcd_update = now;
    }

    // Poll for medication reminders
    if (WiFi.status() == WL_CONNECTED && (now - last_poll > POLL_INTERVAL || last_poll == 0)) {
        poll_medication_reminders();
        last_poll = now;
    }

    delay(50);
}
