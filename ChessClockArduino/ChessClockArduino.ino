#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "secrets.h"
#include "webpage.h"

const char* DEFAULT_STUDY_ID = "3G7mnQhk"; // lichess study ID
const uint32_t LOW_TIME_MS = 10000;
const unsigned long SLEEP_AFTER_MS = 5UL * 60UL * 1000UL;
const unsigned long LONG_PRESS_MS = 800;
const int BATTERY_EMPTY_MV = 3300;
const int BATTERY_FULL_MV  = 4500;

// pins
#define PIN_BATTERY 1
#define PIN_OLED_L_SDA 4
#define PIN_OLED_L_SCL 5
#define PIN_OLED_R_SDA 6
#define PIN_OLED_R_SCL 7
#define PIN_KEY_LEFT 8
#define PIN_KEY_RIGHT 9
#define PIN_KEY_MINUS 10
#define PIN_KEY_START 11
#define PIN_KEY_PLUS 12
#define PIN_BUZZER 13

// screens
const uint8_t OLED_ADDRESS = 0x3C;
Adafruit_SSD1306 oledLeft(128, 64, &Wire, -1);
Adafruit_SSD1306 oledRight(128, 64, &Wire1, -1);

// time controls
enum ClockMode : uint8_t { MODE_INCREMENT = 0, MODE_DELAY = 1 };
enum PlayerColor : uint8_t { COLOR_WHITE = 0, COLOR_BLACK = 1, COLOR_NONE = 2 };
enum Side : uint8_t { SIDE_LEFT = 0, SIDE_RIGHT = 1 };
const char* const COLOR_NAMES[2] = {"WHITE", "BLACK"};

struct Preset {
  const char* name;
  uint16_t minutes;
  uint16_t bonusSec;
};
const Preset PRESETS[] = {
  {"Bullet 1+0", 1, 0},
  {"Blitz 3+0", 3, 0},
  {"Blitz 3+2", 3, 2},
  {"Blitz 5+0", 5, 0},
  {"Blitz 5+3", 5, 3},
  {"Rapid 10+0", 10, 0},
  {"Rapid 10+5", 10, 5},
  {"Rapid 15+10", 15, 10},
  {"Classic 30+0", 30, 0},
};
const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);
const int CUSTOM_PRESET = NUM_PRESETS;

struct Settings {
  uint8_t presetIndex = 4; // default is 5+3
  uint16_t customWhiteMin = 5;
  uint16_t customBlackMin = 5;
  uint16_t customBonusSec = 3;
  uint8_t customMode = MODE_INCREMENT;
  uint8_t whiteSide = SIDE_LEFT;
  bool soundOn = true;
} settings;

struct TimeControl {
  uint32_t whiteMs;
  uint32_t blackMs;
  uint16_t bonusSec;
  uint8_t mode;
};

// game state
enum AppState { STATE_READY, STATE_RUNNING, STATE_PAUSED, STATE_GAME_OVER, STATE_MENU, STATE_WIFI };
AppState appState = STATE_READY;

uint32_t remainingMs[2] = {0, 0};
uint8_t activeColor = COLOR_WHITE;
uint8_t flaggedColor = COLOR_NONE;
uint32_t lastTickMs = 0;
uint32_t delayLeftMs = 0;
bool lowTimeWarned[2] = {false, false};

const int MAX_PLIES = 300;
const int MAX_SAVED_GAMES = 5;
const uint32_t GAME_MAGIC = 0xC10CC10C;
struct GameRecord {
  uint32_t magic;
  uint32_t number;
  uint32_t whiteBaseMs;
  uint32_t blackBaseMs;
  uint16_t bonusSec;
  uint8_t mode;
  uint8_t whiteSide;
  char result[8];
  uint16_t plyCount;
  uint32_t clockMs[MAX_PLIES];
};
GameRecord currentGame;
GameRecord loadedGame;

Preferences prefs;

struct Button {
  uint8_t pin;
  bool stableDown;
  bool lastRead;
  unsigned long changedAt;
  unsigned long pressedAt;
  unsigned long nextRepeatAt;
  bool longDone;
  bool pressedEvent;
  bool shortEvent;
  bool longEvent;
  bool repeatEvent;
};

struct ScreenContent {
  String topLeft;
  String topRight;
  String big;
  uint8_t bigSize = 3;
  String bottomLeft;
  String bottomRight;
  bool highlight = false;
};

// keys
const uint8_t PLAYER_KEY_PINS[2] = {PIN_KEY_LEFT, PIN_KEY_RIGHT};
const unsigned long KEY_SETTLE_MS = 30;
volatile bool playerKeyHeld[2] = {false, false};
volatile unsigned long playerKeyReleasedAt[2] = {0, 0};
volatile unsigned long playerPressAt[2] = {0, 0};
volatile bool playerPressPending[2] = {false, false};

void IRAM_ATTR handlePlayerKeyChange(uint8_t side) {
  unsigned long now = millis();
  bool isDown = digitalRead(PLAYER_KEY_PINS[side]) == LOW;
  if (isDown) {
    if (!playerKeyHeld[side] && now - playerKeyReleasedAt[side] >= KEY_SETTLE_MS) {
      playerKeyHeld[side] = true;
      playerPressAt[side] = now;
      playerPressPending[side] = true;
    }
  } else {
    playerKeyHeld[side] = false;
    playerKeyReleasedAt[side] = now;
  }
}
void IRAM_ATTR onLeftKeyChange() { handlePlayerKeyChange(SIDE_LEFT); }
void IRAM_ATTR onRightKeyChange() { handlePlayerKeyChange(SIDE_RIGHT); }

const unsigned long DEBOUNCE_MS = 20;
const unsigned long REPEAT_DELAY_MS = 450;
const unsigned long REPEAT_RATE_MS = 120;

Button keyMinus = {PIN_KEY_MINUS};
Button keyStart = {PIN_KEY_START};
Button keyPlus = {PIN_KEY_PLUS};

void initButton(Button& b) {
  pinMode(b.pin, INPUT_PULLUP);
  bool down = digitalRead(b.pin) == LOW;
  b.stableDown = down;
  b.lastRead = down;
  b.changedAt = millis();
  b.pressedAt = millis();
  b.nextRepeatAt = 0;
  b.longDone = true;
  b.pressedEvent = b.shortEvent = b.longEvent = b.repeatEvent = false;
}

void updateButton(Button& b, unsigned long now) {
  b.pressedEvent = b.shortEvent = b.longEvent = b.repeatEvent = false;
  bool reading = digitalRead(b.pin) == LOW;
  if (reading != b.lastRead) {
    b.lastRead = reading;
    b.changedAt = now;
  }
  if (now - b.changedAt >= DEBOUNCE_MS && reading != b.stableDown) {
    b.stableDown = reading;
    if (reading) {
      b.pressedEvent = true;
      b.pressedAt = now;
      b.longDone = false;
      b.nextRepeatAt = now + REPEAT_DELAY_MS;
    } else if (!b.longDone) {
      b.shortEvent = true;
    }
  }
  if (b.stableDown && !b.longDone && now - b.pressedAt >= LONG_PRESS_MS) {
    b.longDone = true;
    b.longEvent = true;
  }
  if (b.stableDown && b.nextRepeatAt != 0 && now >= b.nextRepeatAt) {
    b.repeatEvent = true;
    b.nextRepeatAt = now + REPEAT_RATE_MS;
  }
}

// menu
enum MenuItem { MENU_WHITE_TIME, MENU_BLACK_TIME, MENU_MODE, MENU_BONUS, MENU_WHITE_SIDE, MENU_SOUND, MENU_WIFI, MENU_EXIT, NUM_MENU_ITEMS };
const char* const MENU_NAMES[NUM_MENU_ITEMS] = {"White time", "Black time", "Mode", "Inc / delay", "White sits", "Sound", "Wi-Fi games", "Save & exit"};
int menuIndex = 0;

// other state
unsigned long lastActivityAt = 0;
int batteryMv = 0;
unsigned long batteryReadAt = 0;
WebServer server(80);
bool wifiConnected = false;
bool wifiConnecting = false;
String wifiIp;
String lastDrawn[2];
unsigned long lastDrawAt = 0;

// beeps
int beepsLeft = 0;
bool beepOn = false;
unsigned long beepNextAt = 0;
uint16_t beepOnMs = 0;
uint16_t beepOffMs = 0;
uint16_t beepFreq = 0;

// helper functions
uint8_t colorOnSide(uint8_t side) {
  return (side == settings.whiteSide) ? COLOR_WHITE : COLOR_BLACK;
}

TimeControl activeTimeControl() {
  TimeControl tc;
  if (settings.presetIndex < NUM_PRESETS) {
    const Preset& p = PRESETS[settings.presetIndex];
    tc.whiteMs = (uint32_t)p.minutes * 60000UL;
    tc.blackMs = tc.whiteMs;
    tc.bonusSec = p.bonusSec;
    tc.mode = MODE_INCREMENT;
  } else {
    tc.whiteMs = (uint32_t)settings.customWhiteMin * 60000UL;
    tc.blackMs = (uint32_t)settings.customBlackMin * 60000UL;
    tc.bonusSec = settings.customBonusSec;
    tc.mode = settings.customMode;
  }
  return tc;
}

String presetLabel() {
  if (settings.presetIndex < NUM_PRESETS) return PRESETS[settings.presetIndex].name;
  String s = "Custom ";
  s += String(settings.customWhiteMin);
  if (settings.customBlackMin != settings.customWhiteMin) s += "/" + String(settings.customBlackMin);
  s += (settings.customMode == MODE_DELAY) ? " d" : "+";
  s += String(settings.customBonusSec);
  return s;
}

String formatClock(uint32_t ms) {
  char buf[16];
  if (ms < 20000UL) {
    snprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)(ms / 1000UL), (unsigned long)((ms % 1000UL) / 100UL));
  } else {
    unsigned long totalSec = ms / 1000UL;
    unsigned long h = totalSec / 3600UL;
    unsigned long m = (totalSec / 60UL) % 60UL;
    unsigned long s = totalSec % 60UL;
    if (h > 0) snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", h, m, s);
    else snprintf(buf, sizeof(buf), "%lu:%02lu", m, s);
  }
  return String(buf);
}

String batteryText() {
  if (batteryMv < 1500) return "USB";
  int pct = map(batteryMv, BATTERY_EMPTY_MV, BATTERY_FULL_MV, 0, 100);
  pct = constrain(pct, 0, 100);
  return "Bat " + String(pct) + "%";
}

void updateBattery(unsigned long now) {
  if (batteryReadAt != 0 && now - batteryReadAt < 5000) return;
  batteryReadAt = now;
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BATTERY);
  batteryMv = (int)(sum / 8) * 2;
}

// sound
void startBeeps(int count, uint16_t onMs, uint16_t offMs, uint16_t freq) {
  if (!settings.soundOn) return;
  beepsLeft = count;
  beepOnMs = onMs;
  beepOffMs = offMs;
  beepFreq = freq;
  beepOn = false;
  beepNextAt = millis();
}

void updateBeeps(unsigned long now) {
  if (beepsLeft == 0 && !beepOn) return;
  if ((long)(now - beepNextAt) < 0) return;
  if (beepOn) {
    ledcWriteTone(PIN_BUZZER, 0);
    beepOn = false;
    beepNextAt = now + beepOffMs;
  } else if (beepsLeft > 0) {
    ledcWriteTone(PIN_BUZZER, beepFreq);
    beepOn = true;
    beepsLeft--;
    beepNextAt = now + beepOnMs;
  }
}

// saving
void loadSettings() {
  prefs.begin("clock", true);
  settings.presetIndex = prefs.getUChar("preset", settings.presetIndex);
  settings.customWhiteMin = prefs.getUShort("cWhite", settings.customWhiteMin);
  settings.customBlackMin = prefs.getUShort("cBlack", settings.customBlackMin);
  settings.customBonusSec = prefs.getUShort("cBonus", settings.customBonusSec);
  settings.customMode = prefs.getUChar("cMode", settings.customMode);
  settings.whiteSide = prefs.getUChar("wSide", settings.whiteSide);
  settings.soundOn = prefs.getBool("sound", settings.soundOn);
  prefs.end();
  if (settings.presetIndex > CUSTOM_PRESET) settings.presetIndex = 4;
  settings.customWhiteMin = constrain(settings.customWhiteMin, 1, 180);
  settings.customBlackMin = constrain(settings.customBlackMin, 1, 180);
  settings.customBonusSec = constrain(settings.customBonusSec, 0, 60);
  if (settings.customMode > MODE_DELAY) settings.customMode = MODE_INCREMENT;
  if (settings.whiteSide > SIDE_RIGHT) settings.whiteSide = SIDE_LEFT;
}

void saveSettings() {
  prefs.begin("clock", false);
  prefs.putUChar("preset", settings.presetIndex);
  prefs.putUShort("cWhite", settings.customWhiteMin);
  prefs.putUShort("cBlack", settings.customBlackMin);
  prefs.putUShort("cBonus", settings.customBonusSec);
  prefs.putUChar("cMode", settings.customMode);
  prefs.putUChar("wSide", settings.whiteSide);
  prefs.putBool("sound", settings.soundOn);
  prefs.end();
}

String gameKey(int slot) {
  return "game" + String(slot);
}

void saveCurrentGame() {
  if (currentGame.plyCount == 0) return;
  prefs.begin("games", false);
  uint32_t number = prefs.getULong("nextNum", 1);
  currentGame.number = number;
  int slot = (int)((number - 1) % MAX_SAVED_GAMES);
  prefs.putBytes(gameKey(slot).c_str(), &currentGame, sizeof(GameRecord));
  prefs.putULong("nextNum", number + 1);
  prefs.end();
}

bool loadGame(int slot, GameRecord& out) {
  if (slot < 0 || slot >= MAX_SAVED_GAMES) return false;
  prefs.begin("games", true);
  size_t len = prefs.getBytes(gameKey(slot).c_str(), &out, sizeof(GameRecord));
  prefs.end();
  return len == sizeof(GameRecord) && out.magic == GAME_MAGIC && out.plyCount <= MAX_PLIES;
}

// clock logic
void resetGame() {
  TimeControl tc = activeTimeControl();
  remainingMs[COLOR_WHITE] = tc.whiteMs;
  remainingMs[COLOR_BLACK] = tc.blackMs;
  activeColor = COLOR_WHITE;
  flaggedColor = COLOR_NONE;
  delayLeftMs = 0;
  lowTimeWarned[COLOR_WHITE] = false;
  lowTimeWarned[COLOR_BLACK] = false;
  currentGame.magic = GAME_MAGIC;
  currentGame.number = 0;
  currentGame.whiteBaseMs = tc.whiteMs;
  currentGame.blackBaseMs = tc.blackMs;
  currentGame.bonusSec = tc.bonusSec;
  currentGame.mode = tc.mode;
  currentGame.whiteSide = settings.whiteSide;
  strncpy(currentGame.result, "*", sizeof(currentGame.result));
  currentGame.plyCount = 0;
  appState = STATE_READY;
}

void startGame(uint32_t atMs) {
  saveSettings();
  activeColor = COLOR_WHITE;
  lastTickMs = atMs;
  delayLeftMs = (currentGame.mode == MODE_DELAY) ? (uint32_t)currentGame.bonusSec * 1000UL : 0;
  appState = STATE_RUNNING;
}

void finishGame(const char* result) {
  strncpy(currentGame.result, result, sizeof(currentGame.result) - 1);
  currentGame.result[sizeof(currentGame.result) - 1] = '\0';
  saveCurrentGame();
  appState = STATE_GAME_OVER;
}

void advanceClock(uint32_t untilMs) {
  if (appState != STATE_RUNNING) return;
  if ((int32_t)(untilMs - lastTickMs) <= 0) return;
  uint32_t passed = untilMs - lastTickMs;
  lastTickMs = untilMs;

  if (delayLeftMs > 0) {
    uint32_t used = min(passed, delayLeftMs);
    delayLeftMs -= used;
    passed -= used;
  }
  if (passed >= remainingMs[activeColor]) {
    remainingMs[activeColor] = 0;
    flaggedColor = activeColor;
    startBeeps(1, 1500, 0, 2000);
    finishGame(activeColor == COLOR_WHITE ? "0-1" : "1-0");
    return;
  }
  remainingMs[activeColor] -= passed;

  if (!lowTimeWarned[activeColor] && remainingMs[activeColor] < LOW_TIME_MS) {
    lowTimeWarned[activeColor] = true;
    startBeeps(2, 80, 80, 3000);
  }
}

void endTurn(uint32_t atMs) {
  if ((int32_t)(atMs - lastTickMs) < 0) atMs = lastTickMs;
  advanceClock(atMs);
  if (appState != STATE_RUNNING) return;

  if (currentGame.mode == MODE_INCREMENT) {
    remainingMs[activeColor] += (uint32_t)currentGame.bonusSec * 1000UL;
  }
  if (currentGame.plyCount < MAX_PLIES) {
    currentGame.clockMs[currentGame.plyCount] = remainingMs[activeColor];
    currentGame.plyCount++;
  }
  activeColor = (activeColor == COLOR_WHITE) ? COLOR_BLACK : COLOR_WHITE;
  lastTickMs = atMs;
  delayLeftMs = (currentGame.mode == MODE_DELAY) ? (uint32_t)currentGame.bonusSec * 1000UL : 0;
}

void handlePlayerKeys() {
  bool pending[2];
  unsigned long pressAt[2];
  noInterrupts();
  for (int side = 0; side < 2; side++) {
    pending[side] = playerPressPending[side];
    pressAt[side] = playerPressAt[side];
    playerPressPending[side] = false;
  }
  interrupts();

  for (int side = 0; side < 2; side++) {
    if (!pending[side]) continue;
    lastActivityAt = millis();
    uint8_t color = colorOnSide(side);
    if (appState == STATE_READY && color == COLOR_BLACK) {
      startGame((uint32_t)pressAt[side]);
    } else if (appState == STATE_RUNNING && color == activeColor) {
      endTurn((uint32_t)pressAt[side]);
    }
  }
}

// menu logic
void enterMenu() {
  if (settings.presetIndex < NUM_PRESETS) {
    settings.customWhiteMin = PRESETS[settings.presetIndex].minutes;
    settings.customBlackMin = PRESETS[settings.presetIndex].minutes;
    settings.customBonusSec = PRESETS[settings.presetIndex].bonusSec;
    settings.customMode = MODE_INCREMENT;
  }
  menuIndex = 0;
  appState = STATE_MENU;
}

void exitMenu() {
  saveSettings();
  resetGame();
}

String menuValueText(int item) {
  switch (item) {
    case MENU_WHITE_TIME: return String(settings.customWhiteMin) + " min";
    case MENU_BLACK_TIME: return String(settings.customBlackMin) + " min";
    case MENU_MODE: return settings.customMode == MODE_DELAY ? "Delay" : "Increment";
    case MENU_BONUS: return String(settings.customBonusSec) + " sec";
    case MENU_WHITE_SIDE: return settings.whiteSide == SIDE_LEFT ? "Left" : "Right";
    case MENU_SOUND: return settings.soundOn ? "On" : "Off";
    case MENU_WIFI: return "Open";
    case MENU_EXIT: return "Save";
  }
  return "";
}

void changeMenuValue(int step) {
  switch (menuIndex) {
    case MENU_WHITE_TIME:
      settings.customWhiteMin = constrain((int)settings.customWhiteMin + step, 1, 180);
      settings.presetIndex = CUSTOM_PRESET;
      break;
    case MENU_BLACK_TIME:
      settings.customBlackMin = constrain((int)settings.customBlackMin + step, 1, 180);
      settings.presetIndex = CUSTOM_PRESET;
      break;
    case MENU_MODE:
      settings.customMode = (settings.customMode == MODE_DELAY) ? MODE_INCREMENT : MODE_DELAY;
      settings.presetIndex = CUSTOM_PRESET;
      break;
    case MENU_BONUS:
      settings.customBonusSec = constrain((int)settings.customBonusSec + step, 0, 60);
      settings.presetIndex = CUSTOM_PRESET;
      break;
    case MENU_WHITE_SIDE:
      settings.whiteSide = (settings.whiteSide == SIDE_LEFT) ? SIDE_RIGHT : SIDE_LEFT;
      break;
    case MENU_SOUND:
      settings.soundOn = !settings.soundOn;
      break;
    default:
      break;
  }
}

// wifi and the web page
void enterWifi() {
  appState = STATE_WIFI;
  wifiConnected = false;
  wifiConnecting = true;
  wifiIp = "";
  drawScreens(millis(), true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiIp = WiFi.localIP().toString();
    server.begin();
  }
  wifiConnecting = false;
  lastDrawn[SIDE_LEFT] = "";
  lastActivityAt = millis();
}

void exitWifi() {
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  resetGame();
}

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

void handleGames() {
  JsonDocument doc;
  doc["studyId"] = DEFAULT_STUDY_ID;
  doc["tokenSet"] = strlen(LICHESS_TOKEN) > 0;
  JsonArray games = doc["games"].to<JsonArray>();
  for (int slot = 0; slot < MAX_SAVED_GAMES; slot++) {
    if (!loadGame(slot, loadedGame)) continue;
    JsonObject g = games.add<JsonObject>();
    g["slot"] = slot;
    g["number"] = loadedGame.number;
    g["plies"] = loadedGame.plyCount;
    g["result"] = loadedGame.result;
    g["whiteBaseMs"] = loadedGame.whiteBaseMs;
    g["blackBaseMs"] = loadedGame.blackBaseMs;
    g["bonusSec"] = loadedGame.bonusSec;
    g["mode"] = loadedGame.mode == MODE_DELAY ? "delay" : "increment";
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleGame() {
  int slot = server.arg("slot").toInt();
  if (!server.hasArg("slot") || !loadGame(slot, loadedGame)) {
    server.send(404, "application/json", "{\"error\":\"Game not found\"}");
    return;
  }
  JsonDocument doc;
  doc["slot"] = slot;
  doc["number"] = loadedGame.number;
  doc["result"] = loadedGame.result;
  doc["whiteBaseMs"] = loadedGame.whiteBaseMs;
  doc["blackBaseMs"] = loadedGame.blackBaseMs;
  doc["bonusSec"] = loadedGame.bonusSec;
  doc["mode"] = loadedGame.mode == MODE_DELAY ? "delay" : "increment";
  JsonArray clocks = doc["clocks"].to<JsonArray>();
  for (int i = 0; i < loadedGame.plyCount; i++) clocks.add(loadedGame.clockMs[i]);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

String urlEncode(const String& text) {
  const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(text.length() * 3);
  for (unsigned int i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool isValidStudyId(const String& id) {
  if (id.length() != 8) return false;
  for (unsigned int i = 0; i < id.length(); i++) {
    if (!isalnum((uint8_t)id[i])) return false;
  }
  return true;
}

void sendExportResult(bool ok, const String& message, const String& url) {
  JsonDocument doc;
  doc["ok"] = ok;
  doc["message"] = message;
  if (url.length() > 0) doc["url"] = url;
  String out;
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

void handleExport() {
  String studyId = server.arg("study");
  String name = server.arg("name");
  String pgn = server.arg("pgn");
  studyId.trim();
  name.trim();

  if (strlen(LICHESS_TOKEN) == 0) { sendExportResult(false, "Add your Lichess token to secrets.h first.", ""); return; }
  if (!isValidStudyId(studyId)) { sendExportResult(false, "The study ID should be 8 letters or numbers.", ""); return; }
  if (pgn.length() == 0 || pgn.length() > 30000) { sendExportResult(false, "The PGN is empty or too long.", ""); return; }
  if (name.length() == 0) name = "Chess clock game";
  if (name.length() > 100) name = name.substring(0, 100);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  String url = "https://lichess.org/api/study/" + studyId + "/import-pgn";
  if (!http.begin(client, url)) { sendExportResult(false, "Could not reach Lichess.", ""); return; }
  http.addHeader("Authorization", String("Bearer ") + LICHESS_TOKEN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "pgn=" + urlEncode(pgn) + "&name=" + urlEncode(name);
  int code = http.POST(body);
  String reply = http.getString();
  http.end();

  if (code == 200) {
    JsonDocument doc;
    String chapterUrl = "https://lichess.org/study/" + studyId;
    if (!deserializeJson(doc, reply)) {
      const char* chapterId = doc["chapters"][0]["id"] | "";
      if (strlen(chapterId) > 0) chapterUrl += "/" + String(chapterId);
    }
    sendExportResult(true, "Exported to Lichess.", chapterUrl);
  } else if (code == 401) {
    sendExportResult(false, "Lichess rejected the token. Check LICHESS_TOKEN and its study:write scope.", "");
  } else if (code == 403 || code == 404) {
    sendExportResult(false, "Study not found, or it isn't yours.", "");
  } else if (code == 429) {
    sendExportResult(false, "Too many requests. Wait a minute and try again.", "");
  } else {
    sendExportResult(false, "Lichess answered with error " + String(code) + ".", "");
  }
}

// drawing
void renderScreen(Adafruit_SSD1306& oled, const ScreenContent& sc) {
  oled.clearDisplay();
  oled.setTextSize(1);
  if (sc.highlight) {
    oled.fillRect(0, 0, 128, 10, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
  } else {
    oled.setTextColor(SSD1306_WHITE);
  }
  oled.setCursor(2, 1);
  oled.print(sc.topLeft);
  oled.setCursor(126 - (int)sc.topRight.length() * 6, 1);
  oled.print(sc.topRight);

  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(sc.bigSize);
  int bigWidth = (int)sc.big.length() * 6 * sc.bigSize;
  int bigX = max(0, (128 - bigWidth) / 2);
  int bigY = 36 - 4 * sc.bigSize;
  oled.setCursor(bigX, bigY);
  oled.print(sc.big);

  oled.setTextSize(1);
  oled.setCursor(2, 56);
  oled.print(sc.bottomLeft);
  oled.setCursor(126 - (int)sc.bottomRight.length() * 6, 56);
  oled.print(sc.bottomRight);
  oled.display();
}

String modeText() {
  if (currentGame.mode == MODE_DELAY) return "d" + String(currentGame.bonusSec) + "s";
  return "+" + String(currentGame.bonusSec) + "s";
}

void buildScreen(uint8_t side, ScreenContent& sc) {
  uint8_t color = colorOnSide(side);
  switch (appState) {
    case STATE_READY:
      sc.topLeft = COLOR_NAMES[color];
      sc.topRight = (side == SIDE_LEFT) ? "READY" : "";
      sc.big = formatClock(remainingMs[color]);
      sc.bottomLeft = (side == SIDE_LEFT) ? presetLabel() : "Press Start";
      sc.bottomRight = (side == SIDE_RIGHT) ? batteryText() : "";
      break;

    case STATE_RUNNING:
    case STATE_PAUSED:
      sc.topLeft = COLOR_NAMES[color];
      if (appState == STATE_PAUSED) sc.topRight = "PAUSED";
      else if (color == activeColor && delayLeftMs > 0) sc.topRight = "d" + String((delayLeftMs + 999) / 1000);
      sc.highlight = (color == activeColor);
      sc.big = formatClock(remainingMs[color]);
      sc.bottomLeft = (side == SIDE_LEFT) ? "Move " + String(currentGame.plyCount / 2 + 1) : modeText();
      sc.bottomRight = (side == SIDE_RIGHT) ? batteryText() : "";
      break;

    case STATE_GAME_OVER:
      sc.topLeft = COLOR_NAMES[color];
      sc.topRight = (color == flaggedColor) ? "FLAG" : "";
      sc.big = formatClock(remainingMs[color]);
      sc.bottomLeft = (side == SIDE_LEFT) ? "Press: new" : "Hold: Wi-Fi";
      sc.bottomRight = (side == SIDE_LEFT) ? String(currentGame.result) : "";
      break;

    case STATE_MENU:
      if (side == SIDE_LEFT) {
        sc.topLeft = MENU_NAMES[menuIndex];
        sc.topRight = String(menuIndex + 1) + "/" + String(NUM_MENU_ITEMS);
        sc.big = menuValueText(menuIndex);
        sc.bigSize = 2;
        sc.bottomLeft = (menuIndex >= MENU_WIFI) ? "Press Start" : "- / + to change";
      } else {
        sc.topLeft = "Settings";
        sc.big = "Press: next item";
        sc.bigSize = 1;
        sc.bottomLeft = "Hold: save & exit";
      }
      break;

    case STATE_WIFI:
      if (side == SIDE_LEFT) {
        sc.topLeft = "Wi-Fi games";
        sc.bigSize = 1;
        if (wifiConnecting) {
          sc.big = "Connecting...";
        } else if (wifiConnected) {
          sc.big = wifiIp;
          sc.bottomLeft = "Open it in a browser";
        } else {
          sc.big = "Wi-Fi failed";
          sc.bottomLeft = "Check secrets.h";
        }
      } else {
        sc.topLeft = "To leave:";
        sc.big = "Hold Start";
        sc.bigSize = 2;
      }
      break;
  }
}

void drawScreens(unsigned long now, bool force) {
  if (!force && now - lastDrawAt < 40) return;
  lastDrawAt = now;
  for (uint8_t side = 0; side < 2; side++) {
    ScreenContent sc;
    buildScreen(side, sc);
    String key = sc.topLeft + "|" + sc.topRight + "|" + sc.big + "|" + String(sc.bigSize) + "|" +
                 sc.bottomLeft + "|" + sc.bottomRight + "|" + String(sc.highlight);
    if (!force && key == lastDrawn[side]) continue;
    lastDrawn[side] = key;
    renderScreen(side == SIDE_LEFT ? oledLeft : oledRight, sc);
  }
}

// sleep
void goToSleep() {
  saveSettings();
  ledcWriteTone(PIN_BUZZER, 0);
  oledLeft.ssd1306_command(SSD1306_DISPLAYOFF);
  oledRight.ssd1306_command(SSD1306_DISPLAYOFF);

  const uint8_t wakePins[3] = {PIN_KEY_LEFT, PIN_KEY_RIGHT, PIN_KEY_START};
  uint64_t mask = 0;
  for (int i = 0; i < 3; i++) {
    gpio_num_t pin = (gpio_num_t)wakePins[i];
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
    mask |= (1ULL << wakePins[i]);
  }
  esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

void checkSleep(unsigned long now) {
  bool canSleep = (appState == STATE_READY || appState == STATE_GAME_OVER || appState == STATE_MENU);
  if (canSleep && now - lastActivityAt > SLEEP_AFTER_MS) goToSleep();
}

// menu keys per state
void handleMenuKeys(unsigned long now) {
  updateButton(keyMinus, now);
  updateButton(keyStart, now);
  updateButton(keyPlus, now);
  if (keyMinus.pressedEvent || keyStart.pressedEvent || keyPlus.pressedEvent) lastActivityAt = now;

  int step = 0;
  if (keyMinus.pressedEvent || keyMinus.repeatEvent) step = -1;
  if (keyPlus.pressedEvent || keyPlus.repeatEvent) step = 1;

  switch (appState) {
    case STATE_READY:
      if (step != 0 && (keyMinus.pressedEvent || keyPlus.pressedEvent)) {
        int count = NUM_PRESETS + 1;
        settings.presetIndex = (uint8_t)((settings.presetIndex + step + count) % count);
        resetGame();
      }
      if (keyStart.shortEvent) startGame((uint32_t)now);
      if (keyStart.longEvent) enterMenu();
      break;

    case STATE_RUNNING:
      if (keyStart.shortEvent || keyStart.longEvent) {
        advanceClock((uint32_t)now);
        if (appState == STATE_RUNNING) appState = STATE_PAUSED;
      }
      break;

    case STATE_PAUSED:
      if (keyStart.shortEvent) {
        lastTickMs = (uint32_t)now;
        appState = STATE_RUNNING;
      }
      if (keyStart.longEvent) finishGame("*");
      break;

    case STATE_GAME_OVER:
      if (keyStart.shortEvent) resetGame();
      if (keyStart.longEvent) enterWifi();
      break;

    case STATE_MENU:
      if (step != 0) changeMenuValue(step);
      if (keyStart.shortEvent) {
        if (menuIndex == MENU_WIFI) { saveSettings(); enterWifi(); }
        else if (menuIndex == MENU_EXIT) exitMenu();
        else menuIndex = (menuIndex + 1) % NUM_MENU_ITEMS;
      }
      if (keyStart.longEvent) exitMenu();
      break;

    case STATE_WIFI:
      if (keyStart.longEvent) exitWifi();
      break;
  }
}

// setup and loop
void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    rtc_gpio_deinit((gpio_num_t)PIN_KEY_LEFT);
    rtc_gpio_deinit((gpio_num_t)PIN_KEY_RIGHT);
    rtc_gpio_deinit((gpio_num_t)PIN_KEY_START);
  }

  initButton(keyMinus);
  initButton(keyStart);
  initButton(keyPlus);
  for (int side = 0; side < 2; side++) {
    pinMode(PLAYER_KEY_PINS[side], INPUT_PULLUP);
    playerKeyHeld[side] = digitalRead(PLAYER_KEY_PINS[side]) == LOW;
  }
  attachInterrupt(digitalPinToInterrupt(PIN_KEY_LEFT), onLeftKeyChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_KEY_RIGHT), onRightKeyChange, CHANGE);

  ledcAttach(PIN_BUZZER, 2000, 8);
  ledcWriteTone(PIN_BUZZER, 0);
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);

  Wire.begin(PIN_OLED_L_SDA, PIN_OLED_L_SCL, 400000);
  Wire1.begin(PIN_OLED_R_SDA, PIN_OLED_R_SCL, 400000);
  bool leftOk = oledLeft.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false);
  bool rightOk = oledRight.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false);
  if (!leftOk) Serial.println("Left OLED not found - check its wiring");
  if (!rightOk) Serial.println("Right OLED not found - check its wiring");

  WiFi.mode(WIFI_OFF);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/games", HTTP_GET, handleGames);
  server.on("/api/game", HTTP_GET, handleGame);
  server.on("/api/export", HTTP_POST, handleExport);

  loadSettings();
  resetGame();
  updateBattery(millis());
  lastActivityAt = millis();
  drawScreens(millis(), true);
}

void loop() {
  unsigned long now = millis();
  handlePlayerKeys();
  advanceClock((uint32_t)millis());
  handleMenuKeys(now);
  updateBeeps(now);
  updateBattery(now);
  if (appState == STATE_WIFI && wifiConnected) server.handleClient();
  drawScreens(now, false);
  checkSleep(now);
  delay(1);
}
