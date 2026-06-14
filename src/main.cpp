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
 *
 * LCD Pin Definitions (from ESP-BOX-3 BSP, confirmed ILI9341 panel):
 *   SPI MOSI  = GPIO6  (FSPI/SPI3_HOST)
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
#define SERVER_BASE     "http://YOUR_SERVER_IP:3000"
#define DEVICE_MAC     "e8:f6:0a:a8:c3:bc"
#define POLL_INTERVAL   60000   // 60s polling interval

// ============================================================
// VOLUME (0-100)
// ============================================================
#define VOLUME          90

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
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 4096000  // 256 * 16000 = MCLK for ES8311
    };
    
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_MCLK,    // MCLK output on GPIO2 - CRITICAL for ES8311
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
    const int16_t amplitude = 16000;  // High volume (VOLUME=90)
    
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
        i2s_write(I2S_PORT, out_buf, samples_to_fill * 4, &bytes_written, portMAX_DELAY);
    }
    
    Serial.printf("[WAV] Playback complete, %zu bytes played\n", pos);
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
static uint16_t _lcd_w = 320;  // Landscape width
static uint16_t _lcd_h = 240;  // Landscape height
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
// Bit-bang SPI diagnostic (bypass ESP-IDF SPI driver)
// Directly toggles GPIO pins to verify LCD connectivity
// ============================================================
static void lcd_bb_cmd(uint8_t cmd) {
    gpio_set_level((gpio_num_t)LCD_DC, 0);  // DC = LOW (command)
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)LCD_SCLK, 0);
        gpio_set_level((gpio_num_t)LCD_MOSI, (cmd >> i) & 1);
        delayMicroseconds(2);
        gpio_set_level((gpio_num_t)LCD_SCLK, 1);
        delayMicroseconds(2);
    }
}

static void lcd_bb_data8(uint8_t d) {
    gpio_set_level((gpio_num_t)LCD_DC, 1);  // DC = HIGH (data)
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)LCD_SCLK, 0);
        gpio_set_level((gpio_num_t)LCD_MOSI, (d >> i) & 1);
        delayMicroseconds(2);
        gpio_set_level((gpio_num_t)LCD_SCLK, 1);
        delayMicroseconds(2);
    }
}

static void lcd_bb_data16(uint16_t d) {
    lcd_bb_data8((uint8_t)(d >> 8));
    lcd_bb_data8((uint8_t)(d & 0xFF));
}

// Bit-bang full ILI9341 init + fill screen RED
// Returns true if successful (screen should show red)
static bool lcd_bb_init_and_test() {
    Serial.println("[BB] Bit-bang SPI LCD diagnostic starting...");
    Serial.printf("[BB] MOSI=GPIO%d, SCLK=GPIO%d, CS=GPIO%d, DC=GPIO%d, RST=GPIO%d\n",
                 LCD_MOSI, LCD_SCLK, LCD_CS, LCD_DC, LCD_RST);
    
    // Configure GPIO pins
    pinMode(LCD_MOSI, OUTPUT);
    pinMode(LCD_SCLK, OUTPUT);
    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_RST, OUTPUT);
    
    // All idle
    gpio_set_level((gpio_num_t)LCD_CS, HIGH);   // CS HIGH (deselect)
    gpio_set_level((gpio_num_t)LCD_SCLK, HIGH);  // CLK idle HIGH
    gpio_set_level((gpio_num_t)LCD_DC, HIGH);
    gpio_set_level((gpio_num_t)LCD_MOSI, LOW);
    
    delay(10);
    
    // Hardware reset
    Serial.println("[BB] Hardware reset...");
    gpio_set_level((gpio_num_t)LCD_RST, HIGH);
    delay(10);
    gpio_set_level((gpio_num_t)LCD_RST, LOW);
    delay(20);
    gpio_set_level((gpio_num_t)LCD_RST, HIGH);
    delay(150);
    
    // Select LCD
    gpio_set_level((gpio_num_t)LCD_CS, LOW);
    
    // Software reset
    Serial.println("[BB] Software reset...");
    lcd_bb_cmd(ILI9341_SWRESET);
    delay(10);
    
    // Sleep out
    Serial.println("[BB] Sleep out...");
    lcd_bb_cmd(ILI9341_SLPOUT);
    delay(150);
    
    // Pixel format: 16-bit
    lcd_bb_cmd(ILI9341_PIXFMT);
    lcd_bb_data8(0x55);
    
    // MADCTL: landscape
    lcd_bb_cmd(ILI9341_MADCTL);
    lcd_bb_data8(0x28);
    
    // Inversion OFF
    lcd_bb_cmd(ILI9341_INVOFF);
    
    // Normal display mode
    lcd_bb_cmd(ILI9341_NORON);
    delay(10);
    
    // Display ON
    lcd_bb_cmd(ILI9341_DISPON);
    delay(10);
    
    Serial.println("[BB] ILI9341 init commands sent via bit-bang");
    
    // Fill screen RED (320x240 pixels)
    Serial.println("[BB] Filling screen RED via bit-bang...");
    
    // Set address window
    lcd_bb_cmd(ILI9341_CASET);
    lcd_bb_data16(0);
    lcd_bb_data16(319);
    lcd_bb_cmd(ILI9341_PASET);
    lcd_bb_data16(0);
    lcd_bb_data16(239);
    lcd_bb_cmd(ILI9341_RAMWR);
    
    // Send RED pixels (0xF800)
    // Send all 320*240 = 76800 pixels
    uint8_t hi = 0xF8, lo = 0x00;  // RED in RGB565
    gpio_set_level((gpio_num_t)LCD_DC, 1);  // DC HIGH for all data
    for (int pixel = 0; pixel < 76800; pixel++) {
        // Send high byte
        for (int i = 7; i >= 0; i--) {
            gpio_set_level((gpio_num_t)LCD_SCLK, 0);
            gpio_set_level((gpio_num_t)LCD_MOSI, (hi >> i) & 1);
            delayMicroseconds(2);
            gpio_set_level((gpio_num_t)LCD_SCLK, 1);
            delayMicroseconds(2);
        }
        // Send low byte
        for (int i = 7; i >= 0; i--) {
            gpio_set_level((gpio_num_t)LCD_SCLK, 0);
            gpio_set_level((gpio_num_t)LCD_MOSI, (lo >> i) & 1);
            delayMicroseconds(2);
            gpio_set_level((gpio_num_t)LCD_SCLK, 1);
            delayMicroseconds(2);
        }
    }
    
    // Deselect
    gpio_set_level((gpio_num_t)LCD_CS, HIGH);
    
    Serial.println("[BB] Bit-bang RED fill complete!");
    Serial.println("[BB] CHECK: If screen is RED, GPIO pins are correct.");
    Serial.println("[BB] CHECK: If screen is still WHITE, GPIO6/7 may not be LCD SPI pins!");
    
    return true;
}

// SPI low-level communication via ESP-IDF (10MHz, SPI3_HOST)
static void lcd_spi_init() {
    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    digitalWrite(LCD_DC, HIGH);
    
    // SPI bus configuration (ESP-IDF style)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_MOSI,       // GPIO6
        .miso_io_num = -1,              // Not used
        .sclk_io_num = LCD_SCLK,       // GPIO7
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 2,   // Max transfer: 1 scanline (320px * 2 bytes)
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK,
    };
    
    // SPI device configuration (fields MUST match ESP-IDF struct declaration order)
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,                         // SPI mode 0
        .duty_cycle_pos = 128,              // 50% duty cycle
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 1,              // Keep CS active 1 bit after xfer
        .clock_speed_hz = 2 * 1000 * 1000, // 2MHz (slow for reliability)
        .input_delay_ns = 0,
        .spics_io_num = LCD_CS,            // GPIO5
        .flags = 0,                    // No special flags (ILI9341 is full-duplex write only)
        .queue_size = 7,
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
    
    Serial.println("[LCD] ESP-IDF SPI3_HOST bus init OK");
}

// Write command (1 byte, D/C=LOW)
static void lcd_write_cmd(uint8_t cmd) {
    spi_transaction_t t = {};
    t.length = 8;             // 1 byte = 8 bits
    t.tx_buffer = &cmd;
    t.user = (void*)0;        // D/C = LOW (command)
    spi_device_transmit(lcd_spi, &t);
}

// Write data byte (1 byte, D/C=HIGH)
static void lcd_write_data8(uint8_t d) {
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &d;
    t.user = (void*)1;        // D/C = HIGH (data)
    spi_device_transmit(lcd_spi, &t);
}

// Write data 16-bit (2 bytes, D/C=HIGH)
static void lcd_write_data16(uint16_t d) {
    uint8_t buf[2] = { (uint8_t)(d >> 8), (uint8_t)(d & 0xFF) };
    spi_transaction_t t = {};
    t.length = 16;
    t.tx_buffer = buf;
    t.user = (void*)1;
    spi_device_transmit(lcd_spi, &t);
}

// Set address window for pixel writes
static void lcd_set_addr(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    lcd_write_cmd(ILI9341_CASET);
    lcd_write_data16(x1);
    lcd_write_data16(x2);
    lcd_write_cmd(ILI9341_PASET);
    lcd_write_data16(y1);
    lcd_write_data16(y2);
    lcd_write_cmd(ILI9341_RAMWR);
}

// ILI9341 initialization sequence (standard + gamma from Adafruit)
static void lcd_init_panel() {
    // Hardware reset
    digitalWrite(LCD_RST, HIGH);
    delay(10);
    digitalWrite(LCD_RST, LOW);
    delay(10);
    digitalWrite(LCD_RST, HIGH);
    delay(120);

    // Software reset
    lcd_write_cmd(ILI9341_SWRESET);
    delay(5);

    lcd_write_cmd(ILI9341_SLPOUT);
    delay(120);

    // Frame rate control (normal mode)
    lcd_write_cmd(ILI9341_FRMCTR1);
    lcd_write_data8(0x01); lcd_write_data8(0x2C); lcd_write_data8(0x2D);

    // Frame rate control (idle mode)
    lcd_write_cmd(ILI9341_FRMCTR2);
    lcd_write_data8(0x01); lcd_write_data8(0x2C); lcd_write_data8(0x2D);

    // Frame rate control (partial mode)
    lcd_write_cmd(ILI9341_FRMCTR3);
    lcd_write_data8(0x01); lcd_write_data8(0x2C); lcd_write_data8(0x2D);
    lcd_write_data8(0x01); lcd_write_data8(0x2C);

    // Display inversion control
    lcd_write_cmd(ILI9341_INVCTR);
    lcd_write_data8(0x07);

    // Power control
    lcd_write_cmd(ILI9341_PWCTR1);
    lcd_write_data8(0xA2);
    lcd_write_cmd(ILI9341_PWCTR2);
    lcd_write_data8(0xC5);
    lcd_write_cmd(ILI9341_PWCTR3);
    lcd_write_data8(0x0A); lcd_write_data8(0x00);
    lcd_write_cmd(ILI9341_PWCTR4);
    lcd_write_data8(0x8A); lcd_write_data8(0x2A);
    lcd_write_cmd(ILI9341_PWCTR5);
    lcd_write_data8(0x8A); lcd_write_data8(0xEE);

    // VCOM control
    lcd_write_cmd(ILI9341_VMCTR1);
    lcd_write_data8(0x0E);

    // Inversion OFF
    lcd_write_cmd(ILI9341_INVOFF);

    // Pixel format: 16-bit/pixel (RGB565)
    lcd_write_cmd(ILI9341_PIXFMT);
    lcd_write_data8(0x55);

    // Gamma correction (positive)
    lcd_write_cmd(ILI9341_GMCTRP1);
    lcd_write_data8(0x02); lcd_write_data8(0x1C); lcd_write_data8(0x07); lcd_write_data8(0x12);
    lcd_write_data8(0x37); lcd_write_data8(0x32); lcd_write_data8(0x29); lcd_write_data8(0x2D);
    lcd_write_data8(0x29); lcd_write_data8(0x25); lcd_write_data8(0x2B); lcd_write_data8(0x39);
    lcd_write_data8(0x00); lcd_write_data8(0x01); lcd_write_data8(0x03); lcd_write_data8(0x10);

    // Gamma correction (negative)
    lcd_write_cmd(ILI9341_GMCTRN1);
    lcd_write_data8(0x03); lcd_write_data8(0x1D); lcd_write_data8(0x07); lcd_write_data8(0x06);
    lcd_write_data8(0x2E); lcd_write_data8(0x2C); lcd_write_data8(0x29); lcd_write_data8(0x2D);
    lcd_write_data8(0x2E); lcd_write_data8(0x2E); lcd_write_data8(0x37); lcd_write_data8(0x3F);
    lcd_write_data8(0x00); lcd_write_data8(0x00); lcd_write_data8(0x02); lcd_write_data8(0x10);

    // Normal display mode
    lcd_write_cmd(ILI9341_NORON);
    delay(10);

    // Memory Access Control: Landscape rotation 1
    // MADCTL: MV=1 (rotate), BGR=1 (0x08)
    lcd_write_cmd(ILI9341_MADCTL);
    lcd_write_data8(0x28);  // MV=0x20 | BGR=0x08

    // Display ON
    lcd_write_cmd(ILI9341_DISPON);
    delay(10);
}

// Fill screen with solid color (using DMA for speed)
static void lcd_fill_screen(uint16_t color) {
    lcd_set_addr(0, 0, _lcd_w - 1, _lcd_h - 1);
    
    // Build one line of pixel data (DMA-capable static buffer)
    static uint8_t line_buf[320 * 2];
    for (int i = 0; i < _lcd_w; i++) {
        line_buf[i * 2] = (uint8_t)(color >> 8);
        line_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }
    
    // Send one line at a time via DMA
    for (int row = 0; row < _lcd_h; row++) {
        spi_transaction_t t = {};
        t.length = _lcd_w * 2 * 8;    // bits
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
    Serial.println("[LCD] Initializing ILI9341 (raw SPI)...");
    
    // Step 0: Bit-bang SPI diagnostic (before ESP-IDF SPI)
    // This tests if GPIO6/7 are actually connected to the LCD panel
    Serial.println("[LCD] === BIT-BANG SPI DIAGNOSTIC ===");
    lcd_bb_init_and_test();
    delay(3000);  // Wait for user to see the result
    Serial.println("[LCD] === END BIT-BANG DIAGNOSTIC ===");
    
    // Step 1: Backlight (set up LEDC first, but keep off until after init)
    ledcSetup(1, 5000, 10);       // Channel 1, 5kHz, 10-bit resolution
    ledcAttachPin(LCD_BACKLIGHT, 1);
    ledcWrite(1, 1023);           // Backlight ON
    Serial.println("[LCD] Backlight ON");
    
    // Step 2: SPI bus
    Serial.println("[LCD] Init SPI bus (SPI3_HOST, 4MHz)...");
    lcd_spi_init();
    delay(100);
    Serial.println("[LCD] SPI bus OK");
    
    // Step 3: ILI9341 panel init
    Serial.println("[LCD] Init ILI9341 panel...");
    lcd_init_panel();
    
    // Step 4: Turn on backlight AFTER panel init and test pattern
    // (test pattern is drawn with backlight off to verify SPI writes)
    delay(10);
    ledcWrite(1, 1023);           // Full brightness (100%)
    Serial.println("[LCD] Backlight ON");
    
    // Step 5: Draw test pattern (diagnostic - verify SPI writes reach panel)
    Serial.println("[LCD] Drawing test pattern...");
    lcd_test_pattern();
    delay(2000);
    
    Serial.println("[LCD] ILI9341 initialized!");
}

static void lcd_show_boot_screen() {
    lcd_fill_screen(LCD_BLACK);
    lcd_set_text_color(LCD_WHITE, LCD_BLACK);
    lcd_set_text_size(3);
    lcd_set_cursor(60, 10);
    lcd_print("ESP-BOX-3");
    lcd_set_text_size(2);
    lcd_set_cursor(40, 50);
    lcd_print("Medication Reminder");
    lcd_set_text_size(1);
    lcd_set_text_color(LCD_CYAN, LCD_BLACK);
    lcd_set_cursor(80, 90);
    lcd_print("Firmware v3.2 + LCD");
    lcd_set_text_color(LCD_YELLOW, LCD_BLACK);
    lcd_set_cursor(60, 120);
    lcd_print("Initializing...");
}

static void lcd_show_wifi_connecting() {
    lcd_set_text_size(1);
    lcd_set_text_color(LCD_YELLOW, LCD_BLACK);
    lcd_fill_rect(40, 120, 240, 16, LCD_BLACK);
    lcd_set_cursor(40, 120);
    lcd_print("Connecting WiFi...");
}

static void lcd_show_wifi_connected(const char* ip) {
    lcd_set_text_size(1);
    lcd_set_text_color(LCD_GREEN, LCD_BLACK);
    lcd_fill_rect(40, 120, 240, 32, LCD_BLACK);
    lcd_set_cursor(40, 120);
    lcd_print("WiFi Connected!");
    lcd_set_cursor(40, 140);
    lcd_printf("IP: %s", ip);
}

static void lcd_show_status() {
    lcd_set_text_size(1);
    lcd_set_text_color(LCD_GREEN, LCD_BLACK);
    lcd_fill_rect(40, 160, 240, 32, LCD_BLACK);
    lcd_set_cursor(40, 160);
    lcd_print("System Ready");
    lcd_set_cursor(40, 180);
    lcd_print("Polling reminders...");
}

static void lcd_show_time() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeinfo);

    lcd_set_text_size(2);
    lcd_set_text_color(LCD_WHITE, LCD_BLACK);
    lcd_fill_rect(80, 80, 160, 24, LCD_BLACK);
    lcd_set_cursor(100, 80);
    lcd_print(time_str);

    lcd_set_text_size(1);
    lcd_set_text_color(LCD_BLUE, LCD_BLACK);
    lcd_fill_rect(90, 106, 140, 14, LCD_BLACK);
    lcd_set_cursor(110, 106);
    lcd_print(date_str);
}

static void lcd_show_reminder(const char* med_name) {
    lcd_fill_screen(LCD_BLACK);
    lcd_set_text_size(2);
    lcd_set_text_color(LCD_RED, LCD_BLACK);
    lcd_set_cursor(30, 20);
    lcd_print("!! MEDICATION !!");

    lcd_set_text_size(3);
    lcd_set_text_color(LCD_YELLOW, LCD_BLACK);
    lcd_set_cursor(10, 60);
    char buf[22];
    strncpy(buf, med_name, 21);
    buf[21] = '\0';
    lcd_print(buf);

    lcd_set_text_size(2);
    lcd_set_text_color(LCD_WHITE, LCD_BLACK);
    lcd_set_cursor(30, 110);
    lcd_print("TAKE NOW!");

    lcd_set_text_size(1);
    lcd_set_text_color(LCD_GREEN, LCD_BLACK);
    lcd_set_cursor(80, 150);
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
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);  // Wait for serial
    
    Serial.println("\n\n====================================");
    Serial.println("  Medication Reminder - ESP-BOX-3");
    Serial.println("  Firmware v3.2 (Audio + LCD)");
    Serial.println("====================================\n");

    // ---- Step 0: Initialize LCD ----
    Serial.println("[INIT] Step 0: LCD (ILI9341)");
    lcd_init();
    lcd_show_boot_screen();

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
    }
    
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

        // Sync NTP time (UTC+8 for China)
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("[INIT] NTP time sync initiated");
    } else {
        Serial.println("\n[INIT] WiFi connection FAILED!");
        lcd_set_text_size(1);
        lcd_set_text_color(LCD_RED, LCD_BLACK);
        lcd_fill_rect(40, 120, 240, 16, LCD_BLACK);
        lcd_set_cursor(40, 120);
        lcd_print("WiFi FAILED!");
    }

    // Show time on LCD
    lcd_show_time();
    lcd_show_status();

    Serial.println("\n[INIT] Setup complete! Starting polling loop...\n");
}

// ============================================================
// LOOP
// ============================================================
unsigned long last_poll = 0;

void loop() {
    unsigned long now = millis();

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

    delay(100);
}
