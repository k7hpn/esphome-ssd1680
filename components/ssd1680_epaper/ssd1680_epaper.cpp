#include "ssd1680_epaper.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace ssd1680_epaper {

static const char *const TAG = "ssd1680_epaper";

// Display dimensions for 2.9" display (128x296)
static const uint16_t WIDTH = 128;
static const uint16_t HEIGHT = 296;
static const uint32_t ALLSCREEN_BYTES = (WIDTH * HEIGHT) / 8;

void SSD1680EPaper::setup() {
  ESP_LOGCONFIG(TAG, "Setting up SSD1680 E-Paper...");
  
  this->dc_pin_->setup();
  this->dc_pin_->digital_write(false);
  
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
    this->reset_pin_->digital_write(true);
  }
  
  if (this->busy_pin_ != nullptr) {
    this->busy_pin_->setup();
  }
  
  this->spi_setup();
  
  // Initialize the display buffer
  this->init_internal_(ALLSCREEN_BYTES);
  memset(this->buffer_, 0xFF, ALLSCREEN_BYTES);  // Start with white
  
  this->initialized_ = false;
  ESP_LOGCONFIG(TAG, "Setup complete, display init deferred to first update");
}

void SSD1680EPaper::dump_config() {
  LOG_DISPLAY("", "SSD1680 E-Paper", this);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  if (this->busy_pin_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Current BUSY state: %s", 
                  this->busy_pin_->digital_read() ? "HIGH (busy)" : "LOW (idle)");
  }
  LOG_UPDATE_INTERVAL(this);
}

void SSD1680EPaper::hw_reset_() {
  if (this->reset_pin_ == nullptr) {
    ESP_LOGW(TAG, "No reset pin configured");
    return;
  }
    
  ESP_LOGD(TAG, "Hardware reset");
  this->reset_pin_->digital_write(true);
  delay(10);
  this->reset_pin_->digital_write(false);
  delay(10);
  this->reset_pin_->digital_write(true);
  delay(10);
  this->wait_until_idle_();
}

void SSD1680EPaper::wait_until_idle_(uint32_t timeout_ms) {
  if (this->busy_pin_ == nullptr)
    return;
    
  // BUSY pin is LOW when idle, HIGH when busy
  uint32_t start = millis();
  while (this->busy_pin_->digital_read()) {
    if (millis() - start > timeout_ms) {
      ESP_LOGD(TAG, "Wait idle timeout after %lu ms (this may be normal)", millis() - start);
      return;
    }
    delay(10);
    App.feed_wdt();
  }
}

void SSD1680EPaper::command_(uint8_t cmd) {
  this->dc_pin_->digital_write(false);
  this->enable();
  this->write_byte(cmd);
  this->disable();
}

void SSD1680EPaper::data_(uint8_t data) {
  this->dc_pin_->digital_write(true);
  this->enable();
  this->write_byte(data);
  this->disable();
}

void SSD1680EPaper::init_display_() {
  ESP_LOGD(TAG, "Initializing display (SSD1680)");
  
  // Hardware reset
  this->hw_reset_();
  
  // Software reset (0x12)
  ESP_LOGD(TAG, "Sending SW Reset (0x12)");
  this->command_(0x12);
  delay(10);
  this->wait_until_idle_();
  
  // Driver output control (0x01) - Set MUX to 296 lines
  ESP_LOGD(TAG, "Setting driver output control");
  this->command_(0x01);
  this->data_(0x27);  // MUX[7:0] = 0x27 (low byte of 295)
  this->data_(0x01);  // MUX[8] = 1 (high byte, 0x127 = 295, so 296 lines)
  this->data_(0x00);  // GD=0, SM=0, TB=0
  
  // Data entry mode setting (0x11) - X increment, Y increment
  ESP_LOGD(TAG, "Setting data entry mode");
  this->command_(0x11);
  this->data_(0x03);  // AM=0, ID[1:0]=11 (X inc, Y inc)
  
  // Set RAM X address start/end position (0x44)
  ESP_LOGD(TAG, "Setting RAM X address");
  this->command_(0x44);
  this->data_(0x00);  // XStart = 0
  this->data_(0x0F);  // XEnd = 15 (128/8 - 1)
  
  // Set RAM Y address start/end position (0x45)
  ESP_LOGD(TAG, "Setting RAM Y address");
  this->command_(0x45);
  this->data_(0x00);  // YStart low byte = 0
  this->data_(0x00);  // YStart high byte = 0
  this->data_(0x27);  // YEnd low byte = 0x27 (295 & 0xFF)
  this->data_(0x01);  // YEnd high byte = 0x01 (295 >> 8)
  
  // Border waveform control (0x3C)
  ESP_LOGD(TAG, "Setting border waveform");
  this->command_(0x3C);
  this->data_(0x05);  // Follow LUT, LUT1 (white border)
  
  // Temperature sensor control (0x18) - use internal sensor
  ESP_LOGD(TAG, "Setting temperature sensor");
  this->command_(0x18);
  this->data_(0x80);  // Internal temperature sensor
  
  // Set RAM X address counter (0x4E)
  this->command_(0x4E);
  this->data_(0x00);
  
  // Set RAM Y address counter (0x4F)
  this->command_(0x4F);
  this->data_(0x00);
  this->data_(0x00);
  
  this->wait_until_idle_();
  
  ESP_LOGD(TAG, "Display initialization complete");
}

void SSD1680EPaper::full_update_() {
  ESP_LOGD(TAG, "Full refresh with 0xF7");
  
  // 0xF7 = Enable clock, Load temperature, Load LUT, Display, Disable Analog, Disable OSC
  // This is the full sequence that actually refreshes the e-paper panel
  this->command_(0x22);
  this->data_(0xF7);
  this->command_(0x20);
  
  // Wait for refresh to complete (e-paper takes 2-4 seconds typically)
  // Note: BUSY pin may not go LOW on some displays, but refresh still works
  uint32_t start = millis();
  while (this->busy_pin_ != nullptr && this->busy_pin_->digital_read()) {
    if (millis() - start > 5000) {  // 5 second timeout
      // This is often normal - BUSY doesn't always go LOW on this display
      ESP_LOGD(TAG, "Update timeout after %lu ms (display likely still updated)", millis() - start);
      break;
    }
    delay(100);
    App.feed_wdt();
  }
  ESP_LOGD(TAG, "Display update completed in %lu ms", millis() - start);
}

void SSD1680EPaper::display_frame_() {
  ESP_LOGD(TAG, "Writing frame to display");
  
  // Set RAM X address counter
  this->command_(0x4E);
  this->data_(0x00);
  
  // Set RAM Y address counter  
  this->command_(0x4F);
  this->data_(0x00);
  this->data_(0x00);
  
  // Write B/W RAM (0x24) - INVERT data for correct polarity
  // This display: 0xFF in RAM = black pixels, 0x00 = white pixels
  // ESPHome buffer: 0xFF = white (background), bits cleared = black (foreground)
  // So we invert: ~0xFF = 0x00 (white bg), ~cleared = 0xFF (black fg)
  this->command_(0x24);
  for (uint32_t i = 0; i < ALLSCREEN_BYTES; i++) {
    this->data_(~this->buffer_[i]);  // INVERTED for correct polarity
  }
  
  this->wait_until_idle_();
  
  ESP_LOGD(TAG, "Frame written, starting update");
  this->full_update_();
  ESP_LOGD(TAG, "Display update complete");
}

void SSD1680EPaper::update() {
  // Deferred initialization on first update (when logging is working)
  if (!this->initialized_) {
    ESP_LOGI(TAG, "First update - initializing display");
    this->init_display_();
    this->initialized_ = true;
  }
  
  this->do_update_();
  this->display_frame_();
}

void SSD1680EPaper::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= this->get_width_internal() || y < 0 || y >= this->get_height_internal())
    return;
    
  // Calculate buffer position
  // The display is 128x296, stored as 128/8 = 16 bytes per row
  uint32_t pos = (y * (WIDTH / 8)) + (x / 8);
  uint8_t bit = 0x80 >> (x % 8);
  
  if (pos >= ALLSCREEN_BYTES)
    return;
    
  // For e-paper: bit set = white, bit cleared = black
  // (actual display polarity handled by inversion in display_frame_)
  if (color.is_on()) {
    this->buffer_[pos] &= ~bit;  // Clear bit (will become black after inversion)
  } else {
    this->buffer_[pos] |= bit;   // Set bit (will become white after inversion)
  }
}

}  // namespace ssd1680_epaper
}  // namespace esphome
