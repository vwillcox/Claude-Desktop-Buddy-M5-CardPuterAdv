#include <M5Cardputer.h>
#include <esp_mac.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

M5Canvas spr(&M5Cardputer.Display);

// Derive a locally-administered MAC from the factory one and adopt it as
// this chip's base MAC (which BT/WiFi MACs are both derived from). Must
// run before M5Cardputer.begin()/bleInit() so the BT stack picks it up.
// One-time escape hatch: if a host's Bluetooth stack ends up with a stale
// or corrupted bond/cache entry for this device's original address (as
// happened during Cardputer ADV bring-up — connects, then the security
// handshake fails with no passkey ever generated), giving the device a
// new identity sidesteps it without the user having to wipe their Mac's
// whole Bluetooth pairing list.
static void randomizeMac() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  mac[0] |= 0x02;   // locally-administered bit
  mac[0] &= ~0x01;  // keep it a unicast address
  esp_base_mac_addr_set(mac);
}

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple devices
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"

// Cardputer ADV is a fixed-landscape 240x135 screen — no portrait mode,
// no tilt rotation (there's no reason to rotate a keyboard clamshell in
// your hand). The pet lives in a left-hand column; the HUD/approval/clock
// live in a right-hand column alongside it, rather than stacking as they
// did on the old 135x240 portrait stick.
const int W = 240, H = 135;
const int LEFT_W = 120;              // pet column
const int RIGHT_X = LEFT_W;          // HUD/approval/clock column
const int RIGHT_W = W - LEFT_W;
const int CX = W / 2;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → brightness 51..255

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 180000;  // 3 minutes

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
// BMI270 axis signs on the ADV haven't been validated against real hardware
// yet — this mirrors the original stick's threshold shape and will likely
// need sign/threshold tuning once flashed.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5Cardputer.Display.setBrightness(51 + brightLevel * 51); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5Cardputer.Display.wakeup();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5Cardputer.Speaker.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);   // no-op on this layout; see character.cpp
  buddySetPeek(peek);       // no-op on this layout; see buddy.cpp
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "sleep", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
// "clock rot" from the original stick doesn't apply here — this screen never
// rotates — so it's dropped rather than ported.
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 9;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;     // now gates the screen-off attention beep
    case 5: s.hud = !s.hud; break;
    case 6: nextPet(); return;
    case 7: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 8: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first Enter arms (label flips to "really?"), second
// within 3s executes. Navigating away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Shared modal-panel row geometry — compact enough to fit a 9-row settings
// list into a 135px-tall screen.
const int PANEL_ROWH  = 12;
const int PANEL_PAD   = 14;
const int PANEL_HINT_H = 12;

// Single-line footer hint inside a modal panel: "up/dn move  enter ok".
static void drawMenuHints(const Palette& p, int mx, int mw, int hy) {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 8, hy);
  spr.print("up/dn move  enter ok");
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 150, mh = PANEL_PAD + SETTINGS_N * PANEL_ROWH + PANEL_HINT_H;
  int mx = W - mw - 6, my = (H - mh) / 2; if (my < 1) my = 1;   // right-aligned so the pet stays visible on the left
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 6 + i * PANEL_ROWH);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 38, my + 6 + i * PANEL_ROWH);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 10);
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 140, mh = PANEL_PAD + RESET_N * PANEL_ROWH + PANEL_HINT_H;
  int mx = W - mw - 6, my = (H - mh) / 2; if (my < 1) my = 1;   // right-aligned so the pet stays visible on the left
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 6 + i * PANEL_ROWH);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 10);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Power.deepSleep(); break;   // no PMIC rail to cut — deep sleep is the closest we get
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 140, mh = PANEL_PAD + MENU_N * PANEL_ROWH + PANEL_HINT_H;
  int mx = W - mw - 6, my = (H - mh) / 2; if (my < 1) my = 1;   // right-aligned so the pet stays visible on the left
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 6 + i * PANEL_ROWH);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 10);
}

// Bedside-clock screensaver. Cardputer ADV has no RTC chip, so this reads
// a RAM-only offset set from the desktop's BLE time-sync message (see
// dataNowEpoch() in data.h) — correct as long as it's synced since the
// last reboot, otherwise dataRtcValid() is false and this never shows.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static void drawClock() {
  const Palette& p = characterPalette();
  time_t now = dataNowEpoch();
  struct tm lt; gmtime_r(&now, &lt);   // "now" already carries the local tz offset baked in
  char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", lt.tm_hour, lt.tm_min);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02d", lt.tm_sec);
  char dl[16]; snprintf(dl, sizeof(dl), "%s %s %02d", DOW[lt.tm_wday % 7], MON[lt.tm_mon % 12], lt.tm_mday);

  spr.fillRect(RIGHT_X, 0, RIGHT_W, H, p.bg);
  spr.setTextDatum(MC_DATUM);
  int cx = RIGHT_X + RIGHT_W / 2;
  spr.setTextSize(3); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, cx, 42);
  spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, cx, 72);
  spr.setTextSize(1);                                    spr.drawString(dl, cx, 96);
  spr.setTextDatum(TL_DATUM);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

// Persistent screen-level title row ("Info  n/6") matching the PET header,
// then a per-page section label below it.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 32, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 10);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 100); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 45);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 24;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press Enter on a");
    ln("prompt to approve.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "KEYS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("Enter  approve/select");
    spr.setTextColor(p.textDim, p.bg); ln("`      deny/back");
    spr.setTextColor(p.text, p.bg);    ln("Tab    open/close menu");
    spr.setTextColor(p.textDim, p.bg); ln(";  .   navigate/scroll");
    spr.setTextColor(p.text, p.bg);    ln("/      next page");
    spr.setTextColor(p.textDim, p.bg); ln("Space  next screen");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    // Cardputer ADV's power chip is ADC-only — no current sense, no VBUS
    // pin, no temp sensor, so this page is shorter than the original's.
    int vBat_mV = (int)M5.Power.getBatteryVoltage();
    int pct = M5.Power.getBatteryLevel();

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    y += 18;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    y += 4;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 18;

    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 4;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Claude desktop >");
      ln(" Developer > Buddy");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 2;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 2;
    ln("Cardputer ADV");
    ln("(Stamp-S3A / ESP32-S3)");
  }
}

// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int X = RIGHT_X + 4;
  spr.fillRect(RIGHT_X, 0, RIGHT_W, H, p.bg);
  spr.drawFastVLine(RIGHT_X, 0, H, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(X, 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line (~9 chars at 12px in the ~112px column)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 9 ? 2 : 1);
  spr.setCursor(X, toolLen <= 9 ? 18 : 16);
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~18 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(X, 40);
  spr.printf("%.18s", tama.promptHint);
  if (hlen > 18) {
    spr.setCursor(X, 50);
    spr.printf("%.18s", tama.promptHint + 18);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(X, H - 14);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(X, H - 26);
    spr.print("Enter: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(X, H - 14);
    spr.print("`: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 24;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 12;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 14, y + 2, i < mood, moodCol);

  y += 14;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 8;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 14;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 12;
    if (i < en) spr.fillRect(px, y - 2, 8, 6, enCol);
    else spr.drawRect(px, y - 2, 8, 6, p.textDim);
  }

  y += 16;
  spr.fillRoundRect(6, y - 2, 40, 12, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(10, y); spr.printf("Lv %u", stats().level);

  y += 14;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("appr %u  deny %u", stats().approvals, stats().denials);
  y += 9;
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y);
  spr.printf("nap %luh%02lum", nap/3600, (nap/60)%60);
  y += 9;
  auto tokFmt = [&](const char* label, uint32_t v) {
    spr.setCursor(6, y);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tok ", stats().tokens); y += 9;
  tokFmt("today ", tama.tokensToday);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 24;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 8;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 8;
  };
  auto gap = [&]() { y += 1; };

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up, deny lots = down");
  gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens = level up");
  gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap, refills to full");
  gap();

  ln(p.textDim, "idle 3m = screen off, any key wakes");
  gap();

  ln(p.textDim, "space:screens  right:page  tab:menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 4;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 32, y);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int LH = 9, WIDTH = 18;
  const int SHOW = (H - 4) / LH;
  spr.fillRect(RIGHT_X, 0, RIGHT_W, H, p.bg);
  spr.drawFastVLine(RIGHT_X, 0, H, p.textDim);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(RIGHT_X + 4, 4);
    spr.print(tama.msg);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[48][24];
  static uint8_t srcOf[48];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 48; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 48 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(RIGHT_X + 4, 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  Serial.begin(115200);
  randomizeMac();
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);   // 240x135 landscape
  startBt();
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.setColorDepth(16);
  spr.createSprite(W, H);
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5Cardputer.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // No discrete LED on this board. While the screen's on, the animated
  // attention state is already visible; while it's off, chirp periodically
  // so a waiting approval isn't silent.
  static uint32_t lastAttnBeep = 0;
  if (activeState == P_ATTENTION && settings().led && screenOff) {
    if (now - lastAttnBeep > 4000) { lastAttnBeep = now; beep(1400, 50); }
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // --- Keyboard input -------------------------------------------------
  // Keyboard.keysState() is level-triggered (true while held), so track
  // the previous frame's booleans ourselves to get press-edges — the old
  // BtnA/BtnB wasPressed()/pressedFor() equivalents. The registry release
  // of M5Cardputer (1.1.1) has no Fn-layer/esc/arrow fields in KeysState,
  // so deny and navigation use isKeyPressed() on the physical keys that
  // sit where Fn-arrows would be: ` (deny/back), ; . / (up/down/right).
  auto& kbd = M5Cardputer.Keyboard;
  const Keyboard_Class::KeysState& kb = kbd.keysState();
  bool anyDown = kbd.isPressed();
  bool kEsc = kbd.isKeyPressed('`');
  bool kUp = kbd.isKeyPressed(';'), kDown = kbd.isKeyPressed('.'), kRight = kbd.isKeyPressed('/');

  static bool pEnter=false, pEsc=false, pTab=false, pSpace=false;
  static bool pUp=false, pDown=false, pRight=false;
  bool eEnter = kb.enter && !pEnter;
  bool eEsc   = kEsc     && !pEsc;
  bool eTab   = kb.tab   && !pTab;
  bool eSpace = kb.space && !pSpace;
  bool eUp    = kUp      && !pUp;
  bool eDown  = kDown    && !pDown;
  bool eRight = kRight   && !pRight;
  pEnter=kb.enter; pEsc=kEsc; pTab=kb.tab; pSpace=kb.space;
  pUp=kUp; pDown=kDown; pRight=kRight;

  // Waking the screen swallows whatever key woke it, so the same press
  // that lit the screen doesn't also approve/deny/navigate blind.
  static bool swallowKeys = false;
  if (screenOff && anyDown) { wake(); swallowKeys = true; }
  if (swallowKeys && !anyDown) swallowKeys = false;

  if (!swallowKeys) {
    if (anyDown) wake();   // any keypress counts as activity, resets the idle timer

    if (eEnter) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(2400, 30);
        applyReset(resetSel);
      } else if (settingsOpen) {
        beep(2400, 30);
        applySetting(settingsSel);
      } else if (menuOpen) {
        beep(2400, 30);
        menuConfirm();
      }
    }

    if (eEsc) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        statsOnDenial();
        beep(600, 60);
      } else if (resetOpen) {
        resetOpen = false; beep(1800, 30);
      } else if (settingsOpen) {
        settingsOpen = false; characterInvalidate(); beep(1800, 30);
      } else if (menuOpen) {
        menuOpen = false; characterInvalidate(); beep(1800, 30);
      } else if (displayMode != DISP_NORMAL) {
        displayMode = DISP_NORMAL; applyDisplayMode(); beep(1800, 30);
      }
    }

    if (eTab) {
      beep(1800, 30);
      if (resetOpen) resetOpen = false;
      else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
      else { menuOpen = !menuOpen; menuSel = 0; if (!menuOpen) characterInvalidate(); }
      Serial.println(menuOpen ? "menu open" : "menu close");
    }

    if (eSpace && !inPrompt && !menuOpen && !settingsOpen && !resetOpen) {
      beep(1800, 30);
      displayMode = (displayMode + 1) % DISP_COUNT;
      applyDisplayMode();
    }

    if ((eUp || eDown) && (menuOpen || settingsOpen || resetOpen)) {
      beep(1800, 30);
      int8_t d = eDown ? 1 : -1;
      if (resetOpen)          { resetSel    = (resetSel    + RESET_N    + d) % RESET_N;    resetConfirmIdx = 0xFF; }
      else if (settingsOpen)  { settingsSel = (settingsSel + SETTINGS_N + d) % SETTINGS_N; }
      else                    { menuSel     = (menuSel     + MENU_N     + d) % MENU_N; }
    }

    if (eRight && !inPrompt && !menuOpen && !settingsOpen && !resetOpen) {
      if (displayMode == DISP_INFO) {
        beep(2400, 30);
        infoPage = (infoPage + 1) % INFO_PAGES;
      } else if (displayMode == DISP_PET) {
        beep(2400, 30);
        petPage = (petPage + 1) % PET_PAGES;
        applyDisplayMode();
      }
    }

    if (!inPrompt && !menuOpen && !settingsOpen && !resetOpen && displayMode == DISP_NORMAL) {
      if (eDown) { beep(2400, 30); msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1; }
      else if (eUp && msgScroll > 0) { beep(2400, 30); msgScroll--; }
    }
  }

  // Charging-clock screensaver takes the right column over the HUD when
  // nothing's happening and we have a synced software clock (see data.h —
  // there's no RTC chip, so this only shows once the desktop has synced
  // time since the last reboot).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid();

  if (napping || screenOff) {
    // skip sprite render — face-down or screen off
  } else {
    bool showPasskey = blePasskey() != 0;
    bool showPet = displayMode == DISP_NORMAL && !showPasskey;

    if (showPet) {
      if (buddyMode) {
        buddyTick(activeState);
      } else if (characterLoaded()) {
        characterSetState(activeState);
        characterTick();
      } else {
        const Palette& p = characterPalette();
        spr.fillRect(0, 0, LEFT_W, H, p.bg);
        spr.setTextColor(p.textDim, p.bg);
        spr.setTextSize(1);
        if (xferActive()) {
          uint32_t done = xferProgress(), total = xferTotal();
          spr.setCursor(8, 50);
          spr.print("installing");
          spr.setCursor(8, 62);
          spr.printf("%luK / %luK", done/1024, total/1024);
          int barW = LEFT_W - 16;
          spr.drawRect(8, 76, barW, 8, p.textDim);
          if (total > 0) {
            int fill = (int)((uint64_t)barW * done / total);
            if (fill > 1) spr.fillRect(9, 77, fill - 1, 6, p.body);
          }
        } else {
          spr.setCursor(8, 60);
          spr.print("no character");
          spr.setCursor(8, 70);
          spr.print("loaded");
        }
      }
    }

    if (showPasskey) {
      drawPasskey();
    } else if (displayMode == DISP_INFO) {
      drawInfo();
    } else if (displayMode == DISP_PET) {
      drawPet();
    } else if (clocking) {
      drawClock();
    } else if (settings().hud) {
      drawHUD();
    }

    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();

    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5Cardputer.Display.setBrightness(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a key is held → flicker.
  // Unlike the AXP-powered stick we can't tell if we're on USB power, so
  // idle screen-off now fires the same whether plugged in or not.
  if (!screenOff && !inPrompt && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5Cardputer.Display.setBrightness(0);
    M5Cardputer.Display.sleep();
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
