/*
 * DisplayTest - run this FIRST, before the FlightRadar sketch.
 *
 * It checks, one step at a time:
 *   1. the ESP32-S3 starts (Serial Monitor prints)          -> also shows PSRAM
 *   2. the TFT initialises and reports its resolution
 *   3. red / green / blue / white / black full-screen fills
 *   4. rectangles, circles, lines and text
 *   5. the backlight pin (GPIO13): on/off and PWM dimming
 *   6. the controls: encoder A/B/button, MENU, BACK, HOME (live on screen)
 *   7. the amp + speaker: a short rising tone over I2S
 *
 * It needs the SAME TFT_eSPI User_Setup.h as the radar (see TFT_SETUP.md).
 * The pin numbers below must match config.h of the FlightRadar sketch.
 */
#include <TFT_eSPI.h>
#include <driver/i2s.h>

// ---- must match FlightRadar/config.h ------------------------------------
#define TEST_BLK_PIN   13
#define TEST_ENC_A     4
#define TEST_ENC_B     5
#define TEST_ENC_SW    6
#define TEST_BTN_MENU  15
#define TEST_BTN_BACK  16
#define TEST_BTN_HOME  17
#define TEST_I2S_BCLK  18
#define TEST_I2S_LRC   21
#define TEST_I2S_DOUT  7
#define TEST_INVERT    0      // same as DISPLAY_INVERT_COLOURS
#define TEST_ROTATION  0      // same as DISPLAY_ROTATION
// --------------------------------------------------------------------------

#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 1
#endif

TFT_eSPI tft = TFT_eSPI();

static void blInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TEST_BLK_PIN, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TEST_BLK_PIN, 0);
#endif
}
static void blSet(uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TEST_BLK_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

static void banner(const char* title, uint16_t bg, uint16_t fg) {
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(title, tft.width() / 2, tft.height() / 2, 4);
  Serial.println(title);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== FlightRadar display + controls test ===");
  Serial.printf("1. ESP32-S3 running. PSRAM: %s (%u bytes)\n", psramFound() ? "found" : "NOT FOUND (set Tools > PSRAM > OPI PSRAM)",
                (unsigned)ESP.getPsramSize());

  blInit();
  blSet(255);                                   // backlight full on
  pinMode(TEST_ENC_A, INPUT_PULLUP);
  pinMode(TEST_ENC_B, INPUT_PULLUP);
  pinMode(TEST_ENC_SW, INPUT_PULLUP);
  pinMode(TEST_BTN_MENU, INPUT_PULLUP);
  pinMode(TEST_BTN_BACK, INPUT_PULLUP);
  pinMode(TEST_BTN_HOME, INPUT_PULLUP);

  Serial.println("2. Initialising TFT (if the board resets here, see TFT_SETUP.md)...");
  tft.init();
  tft.setRotation(TEST_ROTATION);
  tft.invertDisplay(TEST_INVERT ? true : false);
  Serial.printf("   TFT reports %d x %d pixels (expected 320 x 480)\n", tft.width(), tft.height());

  // Info screen
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("DISPLAY TEST", 10, 10, 4);
  char b[48];
  snprintf(b, sizeof(b), "Resolution: %d x %d", tft.width(), tft.height());
  tft.drawString(b, 10, 50, 2);
  tft.drawString(psramFound() ? "PSRAM: found" : "PSRAM: NOT FOUND", 10, 70, 2);
  tft.drawString("Driver: ST7796S (TFT_eSPI)", 10, 90, 2);
  tft.drawString("Next: colour fills...", 10, 130, 2);
  delay(2500);

  Serial.println("3. Colour fills. Check the colour matches the word!");
  banner("RED", TFT_RED, TFT_WHITE);       delay(1500);
  banner("GREEN", TFT_GREEN, TFT_BLACK);   delay(1500);
  banner("BLUE", TFT_BLUE, TFT_WHITE);     delay(1500);
  banner("WHITE", TFT_WHITE, TFT_BLACK);   delay(1500);
  banner("BLACK", TFT_BLACK, TFT_WHITE);   delay(1000);

  Serial.println("4. Shapes and text.");
  tft.fillScreen(TFT_BLACK);
  const int w = tft.width(), h = tft.height();
  tft.drawRect(0, 0, w, h, TFT_WHITE);                       // border shows the full 320x480 area
  tft.fillRect(10, 10, 60, 40, TFT_RED);
  tft.fillRect(w - 70, 10, 60, 40, TFT_GREEN);
  tft.fillRect(10, h - 50, 60, 40, TFT_BLUE);
  tft.fillRect(w - 70, h - 50, 60, 40, TFT_YELLOW);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("TOP LEFT", 14, 60, 2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("TOP RIGHT", w - 14, 60, 2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("N is up. Text must read", w / 2, 100, 2);
  tft.drawString("normally (not mirrored).", w / 2, 120, 2);
  tft.drawCircle(w / 2, h / 2, 100, TFT_GREEN);
  tft.drawCircle(w / 2, h / 2, 66, TFT_GREEN);
  tft.drawCircle(w / 2, h / 2, 33, TFT_GREEN);
  tft.fillCircle(w / 2, h / 2, 6, TFT_CYAN);
  tft.drawLine(0, 0, w - 1, h - 1, TFT_MAGENTA);
  tft.drawLine(w - 1, 0, 0, h - 1, TFT_MAGENTA);
  tft.drawString("Font 1", w / 2, h / 2 + 120, 1);
  tft.drawString("Font 2", w / 2, h / 2 + 135, 2);
  tft.drawString("Font 4", w / 2, h / 2 + 160, 4);
  delay(4000);

  Serial.println("5. Backlight: fading down and up, then full on.");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.fillRect(20, 140, w - 40, 30, TFT_BLACK);
  tft.drawString("BACKLIGHT TEST", w / 2, 155, 4);
  for (int d = 255; d >= 10; d -= 5) { blSet(d); delay(20); }
  for (int d = 10; d <= 255; d += 5) { blSet(d); delay(20); }
  blSet(255);
  Serial.println("   If the screen did not dim, PWM on GPIO13 is not working: set BACKLIGHT_PWM_ENABLED 0 in config.h.");
  delay(1000);

  Serial.println("6. Controls test: turn/press everything, watch the screen.");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("CONTROLS TEST", 10, 10, 4);
  tft.drawString("Turn the encoder, press it,", 10, 50, 2);
  tft.drawString("and press MENU / BACK / HOME.", 10, 70, 2);
  delay(4000);

  Serial.println("7. Amp + speaker test: a short rising tone, twice.");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("SPEAKER TEST", w / 2, h / 2 - 20, 4);
  tft.drawString("You should hear two beeps.", w / 2, h / 2 + 20, 2);
  i2s_config_t i2sCfg = {};
  i2sCfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sCfg.sample_rate = 16000;
  i2sCfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sCfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sCfg.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S;
  i2sCfg.intr_alloc_flags = 1;
  i2sCfg.dma_buf_count = 4;
  i2sCfg.dma_buf_len = 256;
  if (i2s_driver_install(I2S_NUM_0, &i2sCfg, 0, nullptr) == ESP_OK) {
    i2s_pin_config_t i2sPins = { TEST_I2S_BCLK, TEST_I2S_LRC, TEST_I2S_DOUT, I2S_PIN_NO_CHANGE };
    i2s_set_pin(I2S_NUM_0, &i2sPins);
    for (int rep = 0; rep < 2; rep++) {
      for (int f = 400; f < 1200; f += 4) {               // quick rising sweep = unmistakable "beep"
        int16_t sample = (int16_t)(8000 * sinf(2.0f * PI * f * (millis() % 1000) / 1000.0f));
        int16_t stereo[2] = { sample, sample };
        size_t written;
        i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written, 10);
      }
      delay(300);
    }
    Serial.println("   If you heard nothing: check VIN/GND/BCLK/LRC/DIN wiring and that");
    Serial.println("   the speaker is on SPK+/SPK- (never to GND) - see WIRING.md section 4.");
  } else {
    Serial.println("   i2s_driver_install failed - check the BCLK/LRC/DIN pins above.");
  }
}

static int32_t s_pos = 0;
static uint8_t s_prev = 0xFF;
static uint32_t s_lastDraw = 0;

void loop() {
  static const int8_t T[16] = {0,1,-1,0, -1,0,0,1, 1,0,0,-1, 0,-1,1,0};
  uint8_t cur = (digitalRead(TEST_ENC_A) ? 2 : 0) | (digitalRead(TEST_ENC_B) ? 1 : 0);
  if (s_prev == 0xFF) s_prev = cur;
  if (cur != s_prev) { s_pos += T[(s_prev << 2) | cur]; s_prev = cur; }

  if (millis() - s_lastDraw > 100) {
    s_lastDraw = millis();
    struct Row { const char* name; int pin; } rows[] = {
      {"ENC BUTTON", TEST_ENC_SW}, {"MENU", TEST_BTN_MENU}, {"BACK", TEST_BTN_BACK}, {"HOME", TEST_BTN_HOME}};
    tft.setTextDatum(TL_DATUM);
    char b[40];
    snprintf(b, sizeof(b), "Encoder edges: %6ld  A=%d B=%d  ", (long)s_pos, digitalRead(TEST_ENC_A), digitalRead(TEST_ENC_B));
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(b, 10, 120, 2);
    for (int i = 0; i < 4; i++) {
      bool pressed = digitalRead(rows[i].pin) == LOW;
      tft.fillRect(10, 160 + i * 50, 300, 40, pressed ? TFT_GREEN : TFT_DARKGREY);
      tft.setTextColor(pressed ? TFT_BLACK : TFT_WHITE, pressed ? TFT_GREEN : TFT_DARKGREY);
      tft.setTextDatum(MC_DATUM);
      snprintf(b, sizeof(b), "%s (GPIO %d): %s", rows[i].name, rows[i].pin, pressed ? "PRESSED" : "released");
      tft.drawString(b, 160, 180 + i * 50, 2);
    }
  }
}
