/*
 * FlightRadar - standalone ESP32-S3 ADS-B flight radar
 * Hardware: ESP32-S3-DevKitC (N16R8) + 4.0" ST7796S 480x320 SPI TFT,
 *           rotary encoder + MENU / BACK / HOME buttons,
 *           MAX98357A I2S amp + speaker (military-aircraft announcement).
 *
 * All settings are in config.h. The code lives in the other tabs:
 *   settings   flash storage, units, serial commands
 *   display    TFT_eSPI, sprites, backlight, themes
 *   radar      radar scope drawing
 *   aircraft   aircraft list, motion smoothing, selection, classification
 *   aircraft_route  departure / arrival airport lookup for selected flights
 *   adsb_api   live data download (background task)
 *   wifi_manager  WiFi connection + setup portal + clock
 *   input      encoder / buttons / optional touchscreen (background task)
 *   icons      aircraft symbols
 *   audio      "military aircraft" announcement over I2S (audio_data.h)
 *   train      National Rail departure board (Rail Data Marketplace, background task)
 *   ui         screens, menus, event handling
 *
 * Build settings (Tools menu) are listed in README.md.
 */
#include "config.h"
// The Arduino core's main loop() task gets an 8 KB stack by default (see
// arduino-esp32's main.cpp: getArduinoLoopTaskStackSize() is declared weak
// specifically so a sketch can override it like this). This firmware's UI
// has grown substantially (radar, menus, photo JPEG decode, the train
// board, several editors) and photo loading in particular crashed the
// board right at the point the decode finished - the classic signature of
// a stack overflow silently corrupting memory rather than a clean error.
// Doubling the stack here is the documented way to fix that.
size_t getArduinoLoopTaskStackSize(void) { return 16384; }
#include "settings.h"
#include "display.h"
#include "input.h"
#include "wifi_manager.h"
#include "aircraft.h"
#include "aircraft_photo.h"
#include "aircraft_route.h"
#include "adsb_api.h"
#include "audio.h"
#include "update.h"
#include "train.h"
#include "ui.h"
#if ONBOARD_RGB_LED_PIN >= 0
#include <Adafruit_NeoPixel.h>
static Adafruit_NeoPixel s_onboardLed(1, ONBOARD_RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

void setup() {
#if ONBOARD_RGB_LED_PIN >= 0
  // Force the board's onboard RGB LED off before anything else runs, so it
  // never gets a chance to show its default boot colour.
  s_onboardLed.begin();
  s_onboardLed.setPixelColor(0, 0);
  s_onboardLed.show();
#endif
  settingsBegin();
  Serial.begin(g_settings.serialBaud);
  delay(300);
  Serial.println();
  Serial.println(F(FW_NAME " v" FW_VERSION));
  Serial.printf("PSRAM: %s (%u bytes)\n", psramFound() ? "found" : "NOT FOUND", (unsigned)ESP.getPsramSize());

  inputInit();                                   // pins + input task
  displayInit();
  displaySetBrightness(g_settings.brightness);
  aircraftInit();
  wifiBegin(inputBackHeldAtBoot());              // hold BACK while powering on = setup portal
  adsbInit();
  aircraftPhotoInit();
  aircraftRouteInit();
  audioInit();
  updateInit();
  trainInit();
  uiInit();
  Serial.println(F("Type HELP in the Serial Monitor for setup commands."));
}

void loop() {
  const uint32_t now = millis();

  InputEvent ev;
  while (inputPoll(ev)) uiHandleEvent(ev, now);

  wifiUpdate();
  settingsConsole();
  uiUpdate(now);
  while (aircraftPollAnnouncement()) {                      // military aircraft just became visible
    audioPlayMilitary();
    uiShowMilitaryAlert();
  }
  AircraftService svc;
  while (aircraftPollServiceAnnouncement(svc)) {             // police/coastguard/air ambulance just appeared
    switch (svc) {
      case SVC_POLICE:        audioPlayPolice();       break;
      case SVC_COASTGUARD:    audioPlayCoastguard();   break;
      case SVC_AIR_AMBULANCE: audioPlayAirAmbulance(); break;
      default: break;
    }
  }

  delay(1);                                      // let idle tasks run
}
