// wifi_manager.h - non-blocking WiFi connection, retry logic, setup portal, clock.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>

enum WifiState : uint8_t { WIFI_ST_IDLE = 0, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED, WIFI_ST_DISCONNECTED };

void        wifiBegin(bool forcePortal);     // call once in setup()
void        wifiUpdate();                    // call every loop(); never blocks
void        wifiReconnectNow();              // after credentials changed
WifiState   wifiState();
const char* wifiStateName();                 // "CONNECTING", "CONNECTED", ...
bool        wifiIsConnected();
int         wifiRssi();
void        wifiIpString(char* out, size_t n);
uint16_t    wifiFailCount();

bool        wifiPortalActive();
void        wifiStartPortal(bool manual);
void        wifiStopPortal();
const char* wifiPortalSsid();
void        wifiPortalIpString(char* out, size_t n);

// Clock (set by NTP once WiFi is up)
bool clockGet(struct tm* out);               // false until time is synchronised
void clockFormat(char* out, size_t n, time_t t);   // "HH:MM:SS" local, or "--:--:--"
