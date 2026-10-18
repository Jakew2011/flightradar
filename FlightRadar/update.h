// update.h - optional over-the-air firmware update check + install.
//
// Periodically (and on demand) fetches FW_UPDATE_VERSION_URL, a plain-text
// file containing just the latest version string. If it differs from the
// FW_VERSION this build was compiled with, the UI prompts to install; on
// confirmation this module downloads FW_UPDATE_BIN_URL and flashes it to the
// inactive OTA partition, then reboots into it.
//
// Both URLs live only in config.h - nothing here is user-visible or
// user-editable. If either is left as "" (the default), update checking is
// completely disabled and this module does nothing.
#pragma once
#include <stdint.h>
#include <stddef.h>

enum FwUpdateState : uint8_t {
  FW_UPDATE_IDLE,          // nothing to report (not checked yet, disabled, or up to date)
  FW_UPDATE_CHECKING,      // a version check is in flight
  FW_UPDATE_AVAILABLE,     // a newer version was found; waiting on the user
  FW_UPDATE_DOWNLOADING,   // user confirmed; download + flash in progress
  FW_UPDATE_ERROR          // last check or install attempt failed - see updateLastError()
};

void updateInit();                   // call once in setup(); starts the background task
bool updateIsEnabled();              // false if FW_UPDATE_VERSION_URL/BIN_URL are blank

void updateCheckNow();                 // request an immediate version check (non-blocking)
void updateBeginInstall();             // user confirmed: start download + flash (non-blocking)
void updateDismiss();                  // user said "not now": clears AVAILABLE back to IDLE

FwUpdateState updateState();
void          updateAvailableVersion(char* out, size_t n);  // valid once state != IDLE/CHECKING
uint8_t       updateProgressPercent();                      // valid during DOWNLOADING
void          updateLastError(char* out, size_t n);
