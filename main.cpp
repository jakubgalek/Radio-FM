#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <functional>
#include <vector>
#include <U8g2lib.h>
#include <Audio.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#if __has_include("esp_freertos_hooks.h")
  #include "esp_freertos_hooks.h"
  #define HAVE_IDLE_HOOK 1
#endif

// --- RTC DS3231 (I2C) ---
#define DS3231_I2C_ADDR 0x68

// --- Tuner FM Si4703 (I2C) ---
#define RADIO_RESET_PIN 10
#define RADIO_SDIO_PIN   8   // SDA
#define RADIO_SCLK_PIN   9   // SCL
#define SI4703_I2C_ADDR  0x10

// --- OLED 2.42" SSD1309 128x64 (SPI) ---
#define OLED_SCK_PIN   5
#define OLED_MOSI_PIN  7
#define OLED_CS_PIN    1 
#define OLED_DC_PIN    2
#define OLED_RES_PIN   47

U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI oled(
  U8G2_R0,
  OLED_CS_PIN,
  OLED_DC_PIN,
  OLED_RES_PIN
);

bool oledReady = false;
bool lightMode = false;   // false = ciemny (domyslnie), true = jasny (piksele zamienione)

// Wysyla bufor na ekran; w trybie jasnym zamienia piksele biale z czarnymi.
void oledSend() {
  if (lightMode) {
    uint8_t *buf = oled.getBufferPtr();
    size_t n = (size_t)oled.getBufferTileWidth() * oled.getBufferTileHeight() * 8;
    for (size_t i = 0; i < n; i++) buf[i] = ~buf[i];
  }
  oled.sendBuffer();
}
uint32_t lastOledUpdate = 0;

// --- Audio SD (MP3, biblioteka ESP32-audioI2S) ---
Audio audio;

// --- Pomiar obciazenia CPU (liczenie wywolan watku bezczynnosci obu rdzeni) ---
#ifdef HAVE_IDLE_HOOK
volatile uint32_t idleCounts[2] = {0, 0};
bool idleHookCore0() { idleCounts[0] = idleCounts[0] + 1; return false; }
bool idleHookCore1() { idleCounts[1] = idleCounts[1] + 1; return false; }
float idleBaseRate[2] = {1.0f, 1.0f};   // wywolan na ms, gdy procesor nic nie robi
uint32_t cpuPrevMs = 0;
uint32_t cpuPrevIdle[2] = {0, 0};
int cpuLoadCached = -1;

void cpuMonitorInit() {
  esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
  esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);
  uint32_t t0 = millis();
  uint32_t c0[2] = {idleCounts[0], idleCounts[1]};
  delay(300);   // kalibracja: w tym czasie procesor jest bezczynny
  uint32_t dt = millis() - t0;
  for (int i = 0; i < 2; i++) {
    float rate = (float)(idleCounts[i] - c0[i]) / (float)dt;
    idleBaseRate[i] = rate > 0.0f ? rate : 1.0f;
    cpuPrevIdle[i] = idleCounts[i];
  }
  cpuPrevMs = millis();
}

int cpuLoadPercent() {
  uint32_t now = millis();
  uint32_t dt = now - cpuPrevMs;
  if (dt >= 1000) {
    float idleFrac = 0;
    for (int i = 0; i < 2; i++) {
      uint32_t c = idleCounts[i];
      float f = ((float)(c - cpuPrevIdle[i]) / (float)dt) / idleBaseRate[i];
      idleFrac += constrain(f, 0.0f, 1.0f);
      cpuPrevIdle[i] = c;
    }
    idleFrac /= 2.0f;
    cpuLoadCached = (int)lroundf((1.0f - idleFrac) * 100.0f);
    cpuPrevMs = now;
  }
  return cpuLoadCached;
}
#else
void cpuMonitorInit() { delay(300); }
int cpuLoadPercent() { return -1; }
#endif

enum AudioSource { SOURCE_NONE, SOURCE_SD, SOURCE_RADIO, SOURCE_WEB };
AudioSource currentSource = SOURCE_NONE;

// --- Glosnosc DAC (SD / radio internetowe): stale skoki w dB ---
#define DAC_VOLUME_UI_MAX   100   // liczba pozycji galki dla DAC (FM zostaje 0-15)
#define DAC_DEFAULT_VOLUME  45
#define DAC_STEP_DB         0.5f  // kazdy krok galki = dokladnie tyle dB (stala zmiana glosnosci)
#define DAC_TOP_DB          -10.0f // glosnosc 100 = tyle dB ponizej pelnej skali (zabezpieczenie przed zbyt glosnym dzwiekiem)
#define DAC_LIB_STEPS       255   // rozdzielczosc biblioteki Audio (jesli wersja pozwala)
int dacVolumeSd = DAC_DEFAULT_VOLUME;    // osobna glosnosc dla karty SD
int dacVolumeWeb = DAC_DEFAULT_VOLUME;   // osobna glosnosc dla radia WWW
// (glosnosc FM to osobna zmienna volumeLevel)
int &dacVolRef() { return currentSource == SOURCE_WEB ? dacVolumeWeb : dacVolumeSd; }

// Korektor barwy dla SD i WWW (biblioteka Audio: filtry niskie / srednie / wysokie, w dB)
#define EQ_MIN -40      // zakres wspierany przez biblioteke Audio: -40 ... +6 dB
#define EQ_MAX 6
#define SOUND_ITEM_COUNT 7   // preset, niskie, srednie, wysokie, balans, loudness, wyzeruj
int8_t eqLow = 0, eqMid = 0, eqHigh = 0;   // aktualnie uzywane wzmocnienia pasm
int8_t eqCustom[3] = {0, 0, 0};            // ustawienia wlasne (zapamietane, gdy wybrany jest preset)
int eqPreset = -1;                          // -1 = wlasne, 0.. = numer presetu
int eqBalance = 0;                          // -16 (lewy) ... +16 (prawy)
bool eqLoudness = false;                    // podbicie basow/sopranow przy cichym graniu

struct EqPreset { const char* name; int8_t low, mid, high; };
const EqPreset eqPresets[] = {
  { "Płaski",     0,  0,  0 },
  { "Bass boost", 6,  0, -2 },
  { "Rock",       4, -2,  5 },
  { "Pop",       -1,  3,  1 },
  { "Jazz",       2, -1,  3 },
  { "Mowa",      -6,  4, -2 }
};
const int eqPresetCount = sizeof(eqPresets) / sizeof(eqPresets[0]);
uint32_t audioSettingsDirtyAt = 0;   // zapis glosnosci i korektora do pamieci po chwili spokoju
int lastMainTile = 0;                // ostatnio wybrany kafelek menu glownego (zapamietany)
uint32_t startFmKHz = 0;             // ostatnia czestotliwosc FM z poprzedniego uruchomienia (0 = brak)
int startSource = 2;                 // ostatnie zrodlo: 1 = SD, 2 = FM, 3 = WWW
uint32_t savedFmKHz = 0;             // wartosci ostatnio zapisane do pamieci (do wykrywania zmian)
int savedWebIdx = -1, savedTile = -1, savedSrc = -1;
int dacLibSteps = 21;             // domyslna skala starszych wersji biblioteki

// Wykrywanie setVolumeSteps() w czasie kompilacji (nowsze wersje ESP32-audioI2S).
template <typename T>
auto trySetVolumeSteps(T &a, int n, int) -> decltype(a.setVolumeSteps((uint8_t)n), bool()) {
  a.setVolumeSteps((uint8_t)n);
  return true;
}
template <typename T>
bool trySetVolumeSteps(T &, int, long) { return false; }

int dacLibVolume() {
  int level = dacVolRef();
  if (level <= 0) return 0;
  // Pozycja galki -> docelowa amplituda w dB (0 dB = maksimum). Biblioteka Audio liczy
  // wzmocnienie jako (vol/steps)^2, wiec bierzemy pierwiastek.
  float db = DAC_TOP_DB - (float)(DAC_VOLUME_UI_MAX - level) * DAC_STEP_DB;
  float amplitude = powf(10.0f, db / 20.0f);
  int v = (int)lroundf(sqrtf(amplitude) * dacLibSteps);
  return constrain(v, 1, dacLibSteps);
}
bool sdPaused = false;
bool audioInitialized = false; // I2S ruszy dopiero przy pierwszym odtworzeniu pliku
String currentTrackName = "";
String currentTrackPath = "";
int currentTrackIndex = -1; // indeks aktualnego utworu w tablicy sdFiles[] (-1 = nieznany)

// --- Karta SD (SPI) ---
#define SD_CS_PIN     4
#define SD_SCK_PIN    5
#define SD_MISO_PIN   6
#define SD_MOSI_PIN   7

// --- DAC PCM5102 (I2S) ---
#define I2S_BCLK_PIN  11
#define I2S_LRCK_PIN  12
#define I2S_DOUT_PIN  13

// --- Multiplekser 8-Mux (CD74HC4051) ---
#define MUX_SELECT_PIN 14      // S0
#define MUX_SOURCE_SD    LOW   // S0=0 -> Y0 = wyjscie PCM5102 (SD)
#define MUX_SOURCE_RADIO HIGH  // S0=1 -> Y1 = wyjscie Si4703 (Radio)

#define DEFAULT_FREQ_MHZ 94.5f
#define DEFAULT_VOLUME   1
#define DEFAULT_PLAYBACK_SPEED 1.0f
const float playbackSpeeds[] = { 0.5f, 1.0f, 1.25f, 1.5f };
const int playbackSpeedCount = sizeof(playbackSpeeds) / sizeof(playbackSpeeds[0]);

// --- Enkoder 1: glosnosc ---
#define ENC1_S1   15
#define ENC1_S2   16
#define ENC1_KEY  17

// --- Enkoder 2: menu / stacje ---
#define ENC2_S1   18
#define ENC2_S2   21
#define ENC2_KEY  38

// --- Modul 4 przyciskow, aktywne stanem LOW ---
// Fizycznie od lewej: 4->GPIO42, 3->GPIO41, 2->GPIO40, 1->GPIO39.
#define BTN1_PIN 39   // PREV  (menu: gora | radio: poprz. stacja | SD: +0.5x)
#define BTN2_PIN 40   // NEXT  (menu: dol  | radio: nast. stacja  | SD: -0.5x)
#define BTN3_PIN 41   // odtwarzacz: ZRODLO Radio<->SD | menu: COFNIJ
#define BTN4_PIN 42   // odtwarzacz: wejdz do MENU | menu: WYBIERZ (OK)

#define RADIO_MIN_KHZ 87500
#define RADIO_MAX_KHZ 108000
#define RADIO_STEP_KHZ 100
#define VOLUME_OSD_TIME_MS 1500
#define FM_MAX_STATIONS 20

// --- Pomiar napiecia akumulatora ---
// GPIO3 (ADC1) otrzymuje polowe napiecia ogniwa przez dzielnik 1:1.
#define BATTERY_ADC_PIN 3
#define BATTERY_DIVIDER_RATIO 2.0f
#define BATTERY_MAX_VOLTAGE 4.20f   // gorny limit odczytu (obciecie)
#define BATTERY_FULL_VOLTAGE 4.00f  // od tego napiecia akumulator uznajemy za pelny
#define BATTERY_ADC_SAMPLES 16
#define LOW_BATTERY_WARNING_VOLTAGE 3.10f

// ================== STAN PROGRAMU ==================


uint16_t si4703Regs[16];
bool radioReady = false;
bool radioMuted = false;   // wyciszenie radia - niezalezne od currentSource
bool sdReady = false;
int volumeLevel = DEFAULT_VOLUME;
float playbackSpeed = DEFAULT_PLAYBACK_SPEED;
String cmdBuffer;

// --- Radio internetowe / Wi-Fi: stan globalny ---
bool webMuted = false;            // wyciszenie strumienia internetowego
volatile bool sdEofPending = false;   // koniec utworu zgloszony z biblioteki - obslugiwany w loop()
bool keepWifiOnStop = false;      // zmiana stacji nie wylacza Wi-Fi
String webTitle = "";             // tytul utworu ze strumienia (ICY)
int webStationIndex = 0;
int webPendingIdx = -1;           // stacja wybrana gałką, jeszcze nie uruchomiona
uint32_t webPendingAt = 0;        // kiedy ją uruchomić (po chwili bez kręcenia)
uint32_t webStartMs = 0;          // poczatek (re)laczenia ze stacja
uint32_t webReconnectAt = 0;      // 0 = brak zaplanowanego ponowienia
uint8_t webReconnectTries = 0;
uint8_t webFailStreak = 0;   // ile stacji z rzedu sie nie wczytalo (ochrona przed petla po wszystkich)
bool batteryRefreshRequested = false;
String wifiSsid = "";
String wifiPass = "";

// ================== ENKODERY ==================
struct RotaryEncoder {
  uint8_t pinA;
  uint8_t pinB;
  uint8_t pinKey;
  uint8_t state;
  int8_t accumulator;
  bool lastKey;
  uint32_t lastKeyChange;
  uint8_t detentState;
  bool awaitingDetent;
  uint32_t lastMoveMs;
  volatile int16_t pending;   // obroty zebrane przez watek enkoderow, jeszcze nieodebrane
};

RotaryEncoder enc1 = { ENC1_S1, ENC1_S2, ENC1_KEY, 0, 0, HIGH, 0, 0, false };
RotaryEncoder enc2 = { ENC2_S1, ENC2_S2, ENC2_KEY, 0, 0, HIGH, 0, 0, false };

uint32_t radioFrequencyKHz = (uint32_t)(DEFAULT_FREQ_MHZ * 1000.0f);

bool volumeOverlayVisible = false;
uint32_t volumeOverlayUntil = 0;
int volumeOverlayValue = DEFAULT_VOLUME;
int volumeOverlayMax = 15;

void encoderInit(RotaryEncoder &enc);
void encoderStartTask();
int8_t encoderRead(RotaryEncoder &enc);
bool encoderButtonPressed(RotaryEncoder &enc);
void showVolumeOverlay();
void setRadioFrequencyKHz(uint32_t freqKHz);
void handleEncoders();

// --- Modul 4 przyciskow ---
struct PhysButton {
  uint8_t pin;
  bool lastState;
  uint32_t lastChange;
  uint32_t lastHoldTrigger;
};

PhysButton btn1 = { BTN1_PIN, HIGH, 0, 0 };
PhysButton btn2 = { BTN2_PIN, HIGH, 0, 0 };
PhysButton btn3 = { BTN3_PIN, HIGH, 0, 0 };
PhysButton btn4 = { BTN4_PIN, HIGH, 0, 0 };

void buttonsInit();
bool buttonPressed(PhysButton &b);
void handleButtons();
void stationStep(int8_t dir);
String displayTrackName(const String &name);
void startDeleteConfirm();
void startCleanConfirm();
void confirmEnter();
void toggleSource();
void playTrackByIndex(int idx);
void playNextTrack();
void playPrevTrack();

// ================== ZARZADZANIE UI I MENU ==================
enum UiState { UI_PLAYER, UI_MENU_MAIN, UI_MENU_SD, UI_MENU_RADIO, UI_MENU_DATETIME, UI_MENU_INFO,
               UI_MENU_WEB, UI_MENU_SETTINGS, UI_MENU_WIFI, UI_WIFI_SCAN, UI_TEXT_ENTRY, UI_MENU_DISPLAY, UI_MENU_SOUND, UI_CONFIRM, UI_SD_CLEAN };
UiState currentUi = UI_PLAYER;

// Ekran potwierdzenia (kasowanie utworu, formatowanie karty SD)
enum ConfirmAction : uint8_t { CONFIRM_NONE, CONFIRM_DELETE_FILE, CONFIRM_CLEAN };
ConfirmAction confirmAction = CONFIRM_NONE;
bool confirmYes = false;            // domyslnie zaznaczone "Nie"
UiState confirmReturnUi = UI_MENU_SD;
String confirmName, confirmNote, confirmPath;

int menuIndex = 0;
int menuScroll = 0;
const int itemsPerPage = 4;
int datetimeEditField = 0;
UiState dateTimeReturnUi = UI_MENU_MAIN;   // dokad wracamy po edycji zegara
uint8_t editHour = 0;
uint8_t editMinute = 0;
uint8_t editDay = 1;
uint8_t editMonth = 1;
uint16_t editYear = 2026;
bool dateTimeSaved = false;

String sdFiles[20];
int sdFileCount = 0;

struct SdMenuEntry {
  String name;
  String path;
  bool isDirectory;
};

SdMenuEntry sdMenuEntries[20];
int sdMenuEntryCount = 0;
String currentSdPath = "/";

float fmStations[FM_MAX_STATIONS];
String fmStationNames[FM_MAX_STATIONS];
int fmStationCount = 0;
int currentStationIndex = -1;
String rdsPsText;
char rdsPsBuffer[9] = {};
uint8_t rdsPsSegments = 0;
String rdsPtyText;
String rdsCtDateText;
String rdsCtTimeText;
String rdsPsClockText;

const char* mainMenuItems[] = {
  "Radio FM",
  "Internet",
  "Karta SD",
  "Skanuj",
  "Zegar",
  "Ustaw"
};
const int mainMenuCount = 6;
// Pozycja kafelka -> identyfikator funkcji (ikona + akcja)
const uint8_t mainMenuOrder[] = {2, 6, 1, 3, 4, 7};
const int infoItemCount = 28;
const int infoVisibleRows = 5;

// ================== RTC DS3231 ==================
uint8_t rtcSecond = 0;
uint8_t rtcHour = 17;
uint8_t rtcMinute = 52;
uint8_t rtcDay = 20;
uint8_t rtcMonth = 9;
uint16_t rtcYear = 2026;
bool rtcReady = false;
uint32_t lastRtcRead = 0;

// Deklaracje
void oledUpdate(bool force = false);
void scanSdFiles();
void scanSdDirectory(const String &path);
String sdEntryLabel(const String &path);
void scanFmStations();
int  findClosestStationIndex(uint32_t freqKHz);
void applyVolume();
bool ds3231Begin();
bool ds3231ReadTime();
bool ds3231SetTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);
void printRtcTime();
void menuUp();
void menuDown();
void menuEnter();
void menuBack();
void stopPlayback();
bool startPlayback(const char *pathIn);
void wifiOff();
void recorderStop(bool announce);
void recorderToggle();
void soundAdjust(int d);
void soundEnter(int idx);
void webReturnToFm();
void webStationStep(int8_t dir);
void entryChangeSet(int d);
void entryChangeChar(int d);
void entryBackspace();
void removeLastUtf8Char(String &text);
String findFirstAudioFile();
void deinitAudioI2S();
void ensureAudioInit();
void batteryInit();
void sdInfoUpdate();
extern uint64_t sdTotalBytes;
extern uint64_t sdUsedBytes;
extern bool sdInfoValid;
String formatBytes(uint64_t bytes);
float readBatteryVoltage();
void drawBatteryVoltage();
void drawMenuHint();
void loadPresetStations();
void addStationIfNew(float freqMHz);
void beginDateTimeEdit();
void adjustDateTimeField(int direction);
void saveDateTimeEdit();


// ================== STEROWNIK DS3231 (I2C) ==================

uint8_t bcdToDec(uint8_t value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

uint8_t decToBcd(uint8_t value) {
  return ((value / 10) << 4) | (value % 10);
}

bool ds3231ReadTime() {
  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(0x00); // rejestr sekund
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom((uint8_t)DS3231_I2C_ADDR, (uint8_t)7) != 7) return false;

  uint8_t sec = Wire.read();
  uint8_t min = Wire.read();
  uint8_t hour = Wire.read();
  Wire.read(); // dzien tygodnia
  uint8_t day = Wire.read();
  uint8_t month = Wire.read();
  uint8_t year = Wire.read();

  if (sec & 0x80) return false;

  if (hour & 0x40) {
    bool pm = hour & 0x20;
    uint8_t h = bcdToDec(hour & 0x1F);
    if (h == 12) h = 0;
    if (pm) h += 12;
    rtcHour = h;
  } else {
    rtcHour = bcdToDec(hour & 0x3F);
  }

  rtcSecond = bcdToDec(sec & 0x7F);
  rtcMinute = bcdToDec(min & 0x7F);
  rtcDay = bcdToDec(day & 0x3F);
  rtcMonth = bcdToDec(month & 0x1F);
  rtcYear = 2000 + bcdToDec(year);

  if (rtcHour > 23 || rtcMinute > 59 || rtcSecond > 59 ||
      rtcDay < 1 || rtcDay > 31 || rtcMonth < 1 || rtcMonth > 12) {
    return false;
  }

  return true;
}

bool ds3231SetTime(uint16_t year, uint8_t month, uint8_t day,
                   uint8_t hour, uint8_t minute, uint8_t second) {
  if (year < 2000 || year > 2099 ||
      month < 1 || month > 12 ||
      day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 59) {
    return false;
  }

  int y = year;
  int m = month;
  int d = day;
  if (m < 3) {
    m += 12;
    y--;
  }
  int dow = (d + (13 * (m + 1)) / 5 + y + y / 4 - y / 100 + y / 400 + 6) % 7;
  uint8_t dayOfWeek = (uint8_t)(dow + 1);

  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(0x00);
  Wire.write(decToBcd(second) & 0x7F);
  Wire.write(decToBcd(minute));
  Wire.write(decToBcd(hour));
  Wire.write(decToBcd(dayOfWeek));
  Wire.write(decToBcd(day));
  Wire.write(decToBcd(month));
  Wire.write(decToBcd(year - 2000));

  if (Wire.endTransmission() != 0) return false;

  return ds3231ReadTime();
}

uint8_t dayOfWeek(uint16_t year, uint8_t month, uint8_t day) {
  int y = year;
  int m = month;
  if (m < 3) {
    m += 12;
    y--;
  }
  int dow = (day + (13 * (m + 1)) / 5 + y + y / 4 - y / 100 + y / 400 + 6) % 7;
  return (uint8_t)(dow + 1); // 1 = niedziela
}

bool ds3231Begin() {
  Wire.beginTransmission(DS3231_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    rtcReady = false;
    Serial.println(F("BLAD: Nie wykryto DS3231 (I2C 0x68)."));
    return false;
  }

  if (!ds3231ReadTime()) {
    Serial.println(F("BLAD: DS3231 odpowiada, ale czas jest nieprawidlowy lub zegar zatrzymany."));
    rtcReady = false;
    return false;
  }

  rtcReady = true;
  Serial.printf("DS3231 OK: %02u:%02u:%02u %02u.%02u.%04u\n",
                rtcHour, rtcMinute, rtcSecond,
                rtcDay, rtcMonth, rtcYear);
  return true;
}

void printRtcTime() {
  if (!rtcReady) {
    Serial.println(F("RTC: brak DS3231"));
    return;
  }

  ds3231ReadTime();
  Serial.printf("RTC: %02u:%02u:%02u %02u.%02u.%04u\n",
                rtcHour, rtcMinute, rtcSecond,
                rtcDay, rtcMonth, rtcYear);
}

// ================== STEROWNIK SI4703 (I2C) ==================

void si4703WriteRegisters() {
  Wire.beginTransmission(SI4703_I2C_ADDR);
  for (int r = 0x02; r <= 0x07; r++) {
    Wire.write((uint8_t)(si4703Regs[r] >> 8));
    Wire.write((uint8_t)(si4703Regs[r] & 0xFF));
  }
  Wire.endTransmission();
}

void si4703ReadRegisters() {
  Wire.requestFrom(SI4703_I2C_ADDR, 32);
  uint8_t buf[32];
  uint32_t t0 = millis();
  while (Wire.available() < 32 && millis() - t0 < 200) { /* czekaj */ }
  for (int i = 0; i < 32; i++) {
    buf[i] = Wire.available() ? Wire.read() : 0;
  }
  int idx = 0x0A;
  for (int i = 0; i < 16; i++) {
    si4703Regs[idx] = ((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1];
    idx++;
    if (idx == 0x10) idx = 0;
  }
}

String extractRdsPsClockTime(const String &text) {
  for (int index = 0; index + 4 < text.length(); index++) {
    if (index > 0 && text[index - 1] >= '0' && text[index - 1] <= '9') continue;
    if (text[index] < '0' || text[index] > '9' ||
        text[index + 1] < '0' || text[index + 1] > '9' ||
        text[index + 2] != ':' ||
        text[index + 3] < '0' || text[index + 3] > '9' ||
        text[index + 4] < '0' || text[index + 4] > '9') continue;
    if (index + 5 < text.length() && text[index + 5] >= '0' && text[index + 5] <= '9') continue;

    int hour = (text[index] - '0') * 10 + text[index + 1] - '0';
    int minute = (text[index + 3] - '0') * 10 + text[index + 4] - '0';
    if (hour < 24 && minute < 60) return text.substring(index, index + 5);
  }
  return "";
}

void si4703UpdateRds() {
  if (!radioReady) return;

  si4703ReadRegisters();
  if (!(si4703Regs[0x0A] & (1 << 15))) return;

  uint16_t blockB = si4703Regs[0x0D];
  uint8_t groupType = (blockB >> 12) & 0x0F;
  uint16_t blockD = si4703Regs[0x0F];

  if (groupType == 0) {
    static const char* const programTypes[32] = {
      "brak", "wiadomości", "publicystyka", "informacje", "sport", "edukacja",
      "słuchowisko", "kultura", "nauka", "różne", "pop", "rock",
      "easy listening", "lekka klasyka", "klasyczna", "inna muzyka", "pogoda",
      "finanse", "dzieci", "społeczeństwo", "religia", "telefon", "podróże",
      "czas wolny", "jazz", "country", "muzyka krajowa", "oldies", "folk",
      "dokument", "test alarmu", "alarm"
    };

    uint8_t programType = (blockB >> 5) & 0x1F;
    rdsPtyText = programTypes[programType];

    uint8_t segment = blockB & 0x03;
    char first = (char)(blockD >> 8);
    char second = (char)(blockD & 0xFF);
    rdsPsBuffer[segment * 2] = (first >= 32 && first <= 126) ? first : ' ';
    rdsPsBuffer[segment * 2 + 1] = (second >= 32 && second <= 126) ? second : ' ';
    rdsPsSegments |= (uint8_t)(1 << segment);

    if (rdsPsSegments == 0x0F) {
      rdsPsBuffer[8] = '\0';
      rdsPsText = String(rdsPsBuffer);
      rdsPsText.trim();
      rdsPsClockText = extractRdsPsClockTime(rdsPsText);
    }
  } else if (groupType == 4 && !(blockB & (1 << 11))) {
    uint16_t blockC = si4703Regs[0x0E];
    int modifiedJulianDay = ((blockB & 0x0003) << 15) | (blockC >> 1);
    int hour = ((blockC & 0x0001) << 4) | ((blockD >> 12) & 0x0F);
    int minute = (blockD >> 6) & 0x3F;
    int offsetHalfHours = blockD & 0x1F;
    if (blockD & (1 << 5)) offsetHalfHours = -offsetHalfHours;

    if (hour > 23 || minute > 59 || abs(offsetHalfHours) > 24) return;
    int localMinutes = hour * 60 + minute + offsetHalfHours * 30;
    if (localMinutes < 0) {
      localMinutes += 24 * 60;
      modifiedJulianDay--;
    } else if (localMinutes >= 24 * 60) {
      localMinutes -= 24 * 60;
      modifiedJulianDay++;
    }

    long julianDay = modifiedJulianDay + 2400001L + 68569L;
    long century = 4L * julianDay / 146097L;
    julianDay -= (146097L * century + 3L) / 4L;
    long yearOfCentury = 4000L * (julianDay + 1L) / 1461001L;
    julianDay = julianDay - 1461L * yearOfCentury / 4L + 31L;
    long monthIndex = 80L * julianDay / 2447L;
    int day = (int)(julianDay - 2447L * monthIndex / 80L);
    julianDay = monthIndex / 11L;
    int month = (int)(monthIndex + 2L - 12L * julianDay);
    int year = (int)(100L * (century - 49L) + yearOfCentury + julianDay);

    char dateText[12];
    char timeText[6];
    snprintf(dateText, sizeof(dateText), "%02d.%02d.%04d", day, month, year);
    snprintf(timeText, sizeof(timeText), "%02d:%02d",
         localMinutes / 60, localMinutes % 60);
    rdsCtDateText = dateText;
    rdsCtTimeText = timeText;

    // --- Synchronizacja DS3231 z czasem RDS ---
    // Grupa 4A jest nadawana co minute, dokladnie na poczatku minuty,
    // wiec ustawiamy sekundy na 0. Synchronizujemy tylko poprawnie
    // odebrane dane (bez bledow blokow) i nie czesciej niz co 30 s.
    static uint32_t lastRdsSync = 0;
    bool cleanBlocks = ((si4703Regs[0x0A] >> 9) & 0x03) == 0 &&   // BLERA
                       ((si4703Regs[0x0B] >> 10) & 0x3F) == 0;    // BLERB/C/D
    bool plausible = year >= 2024 && year <= 2099;
    bool editing = (currentUi == UI_MENU_DATETIME);

    if (cleanBlocks && plausible && !editing &&
        (lastRdsSync == 0 || millis() - lastRdsSync >= 30000)) {
      int rdsHour = localMinutes / 60;
      int rdsMinute = localMinutes % 60;

      bool needSync = true;
      if (rtcReady && ds3231ReadTime()) {
        // Aktualny odczyt z RTC - synchronizuj, gdy data/godzina sie
        // roznia albo sekundy odbiegaja od poczatku minuty o wiecej niz 3 s.
        needSync = rtcYear != year || rtcMonth != month || rtcDay != day ||
                   rtcHour != rdsHour || rtcMinute != rdsMinute ||
                   rtcSecond > 3;
      }

      if (needSync) {
        if (ds3231SetTime((uint16_t)year, (uint8_t)month, (uint8_t)day,
                          (uint8_t)rdsHour, (uint8_t)rdsMinute, 0)) {
          rtcReady = true;
          lastRdsSync = millis();
          Serial.printf("RDS: zsynchronizowano zegar DS3231 -> %s %s\n",
                        dateText, timeText);
        } else {
          Serial.println(F("BLAD: Synchronizacja DS3231 z RDS nie powiodla sie."));
        }
      } else {
        lastRdsSync = millis();
      }
    }
  }
}

bool si4703Begin() {
  pinMode(RADIO_RESET_PIN, OUTPUT);
  pinMode(RADIO_SDIO_PIN, OUTPUT);
  digitalWrite(RADIO_SDIO_PIN, LOW);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(2);
  digitalWrite(RADIO_RESET_PIN, HIGH);
  delay(2);

  Wire.begin(RADIO_SDIO_PIN, RADIO_SCLK_PIN);
  Wire.setClock(100000);

  si4703ReadRegisters();
  si4703Regs[0x07] = 0x8100;
  si4703WriteRegisters();
  delay(500);

  si4703ReadRegisters();
  si4703Regs[0x02] = 0x4001;
  si4703Regs[0x04] |= (1 << 12);
  si4703Regs[0x04] |= (1 << 11);
  si4703Regs[0x05] |= (1 << 4);
  si4703Regs[0x05] &= 0xFFF0;
  si4703Regs[0x05] |= (volumeLevel & 0x0F);
  si4703WriteRegisters();
  delay(110);

  si4703ReadRegisters();
  Serial.printf("Si4703 DEVICEID=0x%04X CHIPID=0x%04X\n", si4703Regs[0x00], si4703Regs[0x01]);
  radioReady = (si4703Regs[0x00] != 0x0000 && si4703Regs[0x00] != 0xFFFF);
  return radioReady;
}

void si4703SetMute(bool mute) {
  si4703ReadRegisters();
  if (mute) si4703Regs[0x02] &= ~(1 << 14);
  else      si4703Regs[0x02] |=  (1 << 14);
  si4703WriteRegisters();
}

void si4703SetVolume(int vol) {
  vol = constrain(vol, 0, 15);
  si4703ReadRegisters();
  si4703Regs[0x05] &= 0xFFF0;
  si4703Regs[0x05] |= vol;
  si4703WriteRegisters();
}

void si4703SetFrequencyKHz(uint32_t freqKHz) {
  int channel = (int)((freqKHz - 87500) / 100);
  rdsPsText = "";
  memset(rdsPsBuffer, 0, sizeof(rdsPsBuffer));
  rdsPsSegments = 0;
  rdsPtyText = "";
  rdsCtDateText = "";
  rdsCtTimeText = "";
  rdsPsClockText = "";

  si4703ReadRegisters();
  si4703Regs[0x03] &= 0xFE00;
  si4703Regs[0x03] |= (channel & 0x03FF);
  si4703Regs[0x03] |= (1 << 15);
  si4703WriteRegisters();

  uint32_t t0 = millis();
  while (millis() - t0 < 1000) {
    si4703ReadRegisters();
    if (si4703Regs[0x0A] & (1 << 14)) break;
  }
  si4703Regs[0x03] &= ~(1 << 15);
  si4703WriteRegisters();
}

void setRadioFrequencyKHz(uint32_t freqKHz) {
  if (freqKHz < RADIO_MIN_KHZ) freqKHz = RADIO_MIN_KHZ;
  if (freqKHz > RADIO_MAX_KHZ) freqKHz = RADIO_MAX_KHZ;

  radioFrequencyKHz = freqKHz;

  if (!radioReady) return;

  si4703SetFrequencyKHz(radioFrequencyKHz);
  Serial.printf("FM: %.1f MHz\n", radioFrequencyKHz / 1000.0f);
}

float si4703GetFrequencyMHz() {
  // Wartosc z pamieci RAM (aktualizowana przy strojeniu i seek/skanowaniu)
  return radioFrequencyKHz / 1000.0f;
}

bool si4703Seek(bool up) {
  rdsPsText = "";
  memset(rdsPsBuffer, 0, sizeof(rdsPsBuffer));
  rdsPsSegments = 0;
  rdsPtyText = "";
  rdsCtDateText = "";
  rdsCtTimeText = "";
  rdsPsClockText = "";

  si4703ReadRegisters();
  si4703Regs[0x02] |= (1 << 10);
  if (up) si4703Regs[0x02] |=  (1 << 9);
  else    si4703Regs[0x02] &= ~(1 << 9);
  si4703Regs[0x02] |= (1 << 8);
  si4703WriteRegisters();

  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    si4703ReadRegisters();
    if (si4703Regs[0x0A] & (1 << 14)) break;
  }
  bool failed = si4703Regs[0x0A] & (1 << 13);

  // Odczytaj rzeczywiscie nastrojony kanal - wazne dla wyswietlania I skanowania.
  // Aktualizujemy zawsze (nawet gdy "failed"), bo tuner i tak gdzies "wyladowal".
  int channel = si4703Regs[0x0B] & 0x03FF;
  radioFrequencyKHz = 87500 + (uint32_t)channel * 100;

  si4703Regs[0x02] &= ~(1 << 8);
  si4703WriteRegisters();

  return !failed;
}

// ================== GLOSNOSC (RADIO + MP3) ==================

bool dacIsActiveSource() {
  return currentSource == SOURCE_SD || currentSource == SOURCE_WEB;
}
int activeVolume()    { return dacIsActiveSource() ? dacVolRef() : volumeLevel; }
int activeVolumeMax() { return dacIsActiveSource() ? DAC_VOLUME_UI_MAX : 15; }
// Glosnosc w procentach: DAC co 2% (50 krokow), FM ma sprzetowo 15 poziomow.
int activeVolumePercent() {
  if (dacIsActiveSource()) return dacVolRef() * 100 / DAC_VOLUME_UI_MAX;
  return volumeLevel;   // FM: prawdziwe 15 stopni tunera
}

// Wysyla ustawienia korektora i balansu do biblioteki (z loudness zaleznym od glosnosci).
// Zmiana jest wysylana tylko, gdy wartosci sie zmienily (mniej "klikniec").
int8_t eqAppliedLow = 127, eqAppliedMid = 127, eqAppliedHigh = 127, eqAppliedBal = 127;

void eqApply() {
  if (!audioInitialized) return;
  int low = eqLow, mid = eqMid, high = eqHigh;
  if (eqLoudness) {
    int v = dacVolRef();                                   // 0..100
    float boost = v < 50 ? (50 - v) * 0.12f : 0.0f;       // do +6 dB przy bardzo cichym graniu
    low += (int)lroundf(boost);
    high += (int)lroundf(boost * 0.5f);
  }
  low = constrain(low, EQ_MIN, EQ_MAX);
  mid = constrain(mid, EQ_MIN, EQ_MAX);
  high = constrain(high, EQ_MIN, EQ_MAX);
  if (low != eqAppliedLow || mid != eqAppliedMid || high != eqAppliedHigh) {
    audio.setTone((int8_t)low, (int8_t)mid, (int8_t)high);
    eqAppliedLow = low; eqAppliedMid = mid; eqAppliedHigh = high;
  }
  if (eqBalance != eqAppliedBal) {
    audio.setBalance((int8_t)eqBalance);
    eqAppliedBal = eqBalance;
  }
}

void applyVolume() {
  si4703SetVolume(volumeLevel);
  bool silent = (webMuted && currentSource == SOURCE_WEB);
  audio.setVolume(silent ? 0 : dacLibVolume());
  eqApply();   // loudness zalezy od glosnosci
}

// ================== ODTWARZACZ MP3 (KARTA SD) ==================

void stopPlayback() {
  bool wasWeb = (currentSource == SOURCE_WEB);
  if (wasWeb) recorderStop(true);   // zamknij nagranie przed wylaczeniem Wi-Fi
  if (currentSource == SOURCE_SD || wasWeb) {
    audio.stopSong();
    deinitAudioI2S();   // zatrzymaj zegary I2S i uspij PCM5102, aby nie zaklocaly FM
  }
  sdPaused = false;
  currentSource = SOURCE_NONE;
  if (wasWeb) {
    webTitle = "";
    webReconnectAt = 0;
    webReconnectTries = 0;
    webPendingIdx = -1;
    // Wi-Fi zaklóca odbiór FM - wylaczamy po radiu internetowym.
    if (!keepWifiOnStop) wifiOff();
  }
}

String buildFullSdPath(const String &basePath, const String &entryName) {
  if (entryName.startsWith("/")) {
    return entryName;
  }

  if (basePath.length() == 0 || basePath == "/") {
    return "/" + entryName;
  }

  if (basePath.endsWith("/")) {
    return basePath + entryName;
  }

  return basePath + "/" + entryName;
}

bool isHiddenSdDirectory(const String &path) {
  String name = sdEntryLabel(path);
  return name.equalsIgnoreCase("data") ||
         name.equalsIgnoreCase("System Volume Information");
}

void scanSdFilesRecursive(File dir, const String &basePath = "") {
  while (File entry = dir.openNextFile()) {
    String entryName = String(entry.name());
    String fullPath = buildFullSdPath(basePath, sdEntryLabel(entryName));

    if (entry.isDirectory()) {
      if (entryName != "/" && entryName != "." && entryName != ".." &&
          !isHiddenSdDirectory(entryName)) {
        scanSdFilesRecursive(entry, fullPath);
      }
    } else {
      String lower = fullPath;
      lower.toLowerCase();
      if (lower.endsWith(".mp3") && sdFileCount < 20) {
        sdFiles[sdFileCount++] = fullPath;
      }
    }

    entry.close();
  }
}

String findFirstAudioFile() {
  if (sdFileCount == 0) {
    scanSdFiles();
  }

  if (sdFileCount > 0) {
    return sdFiles[0];
  }

  return "";
}

void deinitAudioI2S() {
  // Odlaczenie zegarow I2S (BCLK/LRCK/DOUT) od matrycy GPIO ESP32-S3 przy powrocie do radia FM.
  // Eliminuje zaklocenia EMI wnoszone do tunera Si4703 i usypia DAC PCM5102 (brak BCLK -> Power-Down).
  if (!audioInitialized) return;

  pinMode(I2S_BCLK_PIN, OUTPUT);
  digitalWrite(I2S_BCLK_PIN, LOW);

  pinMode(I2S_LRCK_PIN, OUTPUT);
  digitalWrite(I2S_LRCK_PIN, LOW);

  pinMode(I2S_DOUT_PIN, OUTPUT);
  digitalWrite(I2S_DOUT_PIN, LOW);

  // Wymuszenie ponownego wyslania ustawien korektora/balansu przy kolejnym starcie audio
  eqAppliedLow = 127;
  eqAppliedMid = 127;
  eqAppliedHigh = 127;
  eqAppliedBal = 127;

  audioInitialized = false;
}

void ensureAudioInit() {
  // I2S dla PCM5102 uruchamiamy dopiero, gdy naprawde potrzebny (pierwsze MP3 / WWW).
  // Dzieki temu podczas samego sluchania radia zegar I2S (BCLK/LRCK) w ogole
  // nie chodzi, co eliminuje ewentualne zaklocenia wnoszone do tunera FM.
  if (!audioInitialized) {
    audio.setPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
    if (trySetVolumeSteps(audio, DAC_LIB_STEPS, 0)) dacLibSteps = DAC_LIB_STEPS;
    audio.setVolume(dacLibVolume());
    audioInitialized = true;
    eqApply();
  }
}

bool startPlayback(const char *pathIn) {
  String path(pathIn);
  if (!path.startsWith("/")) path = "/" + path;
  path.trim();

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    Serial.printf("BLAD: Plik SD niedostepny: %s\n", path.c_str());
    file.close();
    return false;
  }
  file.close();

  bool returnToRadio = currentSource == SOURCE_RADIO;
  stopPlayback();
  if (radioReady) si4703SetMute(true);
  digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_SD);

  ensureAudioInit();

  if (!audio.connecttoFS(SD, path.c_str())) {
    Serial.printf("BLAD: Nie mozna odtworzyc pliku %s\n", path.c_str());
    currentSource = returnToRadio ? SOURCE_RADIO : SOURCE_NONE;
    if (returnToRadio) {
      if (radioReady) si4703SetMute(false);
      digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
    }
    return false;
  }

  currentSource = SOURCE_SD;
  applyVolume();   // SD ma wlasna zapamietana glosnosc
  sdPaused = false;
  playbackSpeed = DEFAULT_PLAYBACK_SPEED;
  int lastSlash = path.lastIndexOf('/');
  currentTrackName = (lastSlash >= 0) ? path.substring(lastSlash + 1) : path;
  currentTrackPath = path;

  if (radioReady) si4703SetMute(true); // dzwiek plynie teraz z SD
  digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_SD);
  currentUi = UI_PLAYER;

  Serial.printf("Odtwarzam (MP3): %s\n", path.c_str());
  return true;
}

// Wywolywane automatycznie przez biblioteke Audio po zakonczeniu utworu.
void audio_eof_mp3(const char *info) {
  if (currentSource == SOURCE_WEB) return;
  // Nie wolno uruchamiac nastepnego pliku wewnatrz tego callbacku: biblioteka po powrocie
  // jeszcze zeruje swoj stan (dlugosc/pozycje) i nowy utwor zostaje "ogluszony".
  Serial.printf("Koniec utworu: %s\n", info);
  sdEofPending = true;
}

void handleSdEof() {
  if (!sdEofPending) return;
  sdEofPending = false;
  if (currentSource != SOURCE_SD) return;
  stopPlayback();
  if (currentTrackPath.length() > 0) {
    playNextTrack();
    return;
  }
  currentUi = UI_PLAYER;
  oledUpdate(true);
}

void audio_info(const char *info) {
  Serial.print(F("audio_info: "));
  Serial.println(info);
}

// --- Przelaczanie zrodla i utworow - uzywane przez przyciski BTN1-BTN4 ---

void toggleSource() {
  if (currentSource == SOURCE_WEB) {
    // Radio internetowe -> FM
    webReturnToFm();
    Serial.println(F("Zrodlo: RADIO"));
    return;
  }
  if (currentSource == SOURCE_SD) {
    // SD -> Radio: wracamy do radia na ostatnio ustawionej czestotliwosci.
    stopPlayback();
    currentSource = SOURCE_RADIO;
    radioMuted = false;
    if (radioReady) si4703SetMute(false);
    digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
    Serial.println(F("Zrodlo: RADIO"));
  } else {
    // Radio -> SD: gramy ostatnio wybrany utwor, a w jego braku pierwszy
    // plik znaleziony na karcie.
    String toPlay;
    if (currentTrackIndex >= 0 && currentTrackIndex < sdFileCount) {
      toPlay = sdFiles[currentTrackIndex];
    } else {
      if (sdFileCount == 0) scanSdFiles();
      if (sdFileCount > 0) { toPlay = sdFiles[0]; currentTrackIndex = 0; }
    }
    if (toPlay.length() > 0) {
      startPlayback(toPlay.c_str());
      Serial.println(F("Zrodlo: KARTA SD"));
    } else {
      Serial.println(F("BLAD: Brak plikow MP3 na karcie SD."));
    }
  }
  oledUpdate(true);
}

void playTrackByIndex(int idx) {
  if (sdFileCount == 0) return;
  idx %= sdFileCount;
  if (idx < 0) idx += sdFileCount;
  currentTrackIndex = idx;
  startPlayback(sdFiles[idx].c_str());
  oledUpdate(true);
}

void playAdjacentTrack(int direction) {
  if (currentTrackPath.length() == 0) {
    Serial.println(F("BLAD: Brak wybranego utworu."));
    return;
  }

  int slash = currentTrackPath.lastIndexOf('/');
  String directory = (slash <= 0) ? "/" : currentTrackPath.substring(0, slash);
  scanSdDirectory(directory);

  int trackCount = 0;
  int currentIndex = -1;
  for (int i = 0; i < sdMenuEntryCount; i++) {
    if (!sdMenuEntries[i].isDirectory) {
      if (sdMenuEntries[i].path == currentTrackPath) currentIndex = trackCount;
      trackCount++;
    }
  }

  if (trackCount == 0) {
    Serial.println(F("BLAD: Brak plikow MP3 w tym katalogu."));
    return;
  }

  int targetIndex = currentIndex < 0
                      ? (direction > 0 ? 0 : trackCount - 1)
                      : (currentIndex + direction + trackCount) % trackCount;
  for (int i = 0; i < sdMenuEntryCount; i++) {
    if (!sdMenuEntries[i].isDirectory && targetIndex-- == 0) {
      currentSdPath = directory;
      startPlayback(sdMenuEntries[i].path.c_str());
      oledUpdate(true);
      return;
    }
  }
}

void playNextTrack() { playAdjacentTrack(1); }
void playPrevTrack() { playAdjacentTrack(-1); }


// ================== OBSLUGA ENKODEROW ==================

void encoderInit(RotaryEncoder &enc) {
  pinMode(enc.pinA, INPUT_PULLUP);
  pinMode(enc.pinB, INPUT_PULLUP);
  pinMode(enc.pinKey, INPUT_PULLUP);

  enc.state = ((digitalRead(enc.pinA) & 1) << 1) | (digitalRead(enc.pinB) & 1);
  enc.accumulator = 0;
  enc.detentState = enc.state;
  enc.awaitingDetent = false;
  enc.lastKey = digitalRead(enc.pinKey);
  enc.lastKeyChange = millis();
}

int8_t encoderDecode(RotaryEncoder &enc) {
  static const int8_t transitionTable[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };

  uint8_t newState = ((digitalRead(enc.pinA) & 1) << 1) |
                     (digitalRead(enc.pinB) & 1);

  if (enc.awaitingDetent) {
    enc.state = newState;
    if (newState == enc.detentState) {
      enc.awaitingDetent = false;
      enc.accumulator = 0;
    }
    return 0;
  }

  uint8_t index = (enc.state << 2) | newState;
  enc.state = newState;

  int8_t movement = transitionTable[index];
  if (movement == 0) return 0;

  // Resztka niedokonczonego obrotu (np. +2) nie moze "zjadac" pierwszego kroku w druga strone:
  // po chwili bezruchu albo przy zmianie kierunku zerujemy akumulator.
  uint32_t nowMove = millis();
  if (nowMove - enc.lastMoveMs > 120) enc.accumulator = 0;
  enc.lastMoveMs = nowMove;
  if ((movement > 0 && enc.accumulator <= -2) || (movement < 0 && enc.accumulator >= 2)) enc.accumulator = 0;

  enc.accumulator += movement;
  int8_t negativeThreshold = (&enc == &enc2 &&
                              (currentUi == UI_MENU_MAIN || currentUi == UI_MENU_INFO))
                               ? -2 : -4;

  if (enc.accumulator >= 4) {
    enc.accumulator = 0;
    return 1;
  }

  if (enc.accumulator <= negativeThreshold) {
    enc.accumulator = 0;
    if (negativeThreshold == -2) enc.awaitingDetent = true;
    return -1;
  }

  return 0;
}

// Odczyt pokretel dziala w osobnym watku co 1 ms, wiec obroty nie gina, gdy petla glowna
// zostaje zablokowana (np. slabe Wi-Fi i oczekiwanie na dane strumienia).
static portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;

static void encoderTask(void *) {
  for (;;) {
    int8_t s1 = encoderDecode(enc1);
    int8_t s2 = encoderDecode(enc2);
    if (s1 || s2) {
      portENTER_CRITICAL(&encMux);
      enc1.pending += s1;
      enc2.pending += s2;
      portEXIT_CRITICAL(&encMux);
    }
    vTaskDelay(1);
  }
}

void encoderStartTask() {
  xTaskCreatePinnedToCore(encoderTask, "encoders", 3072, nullptr, 6, nullptr, 1);
}

// Zwraca sume obrotow zebranych od ostatniego wywolania (dodatnia = w prawo).
int8_t encoderRead(RotaryEncoder &enc) {
  portENTER_CRITICAL(&encMux);
  int16_t v = enc.pending;
  enc.pending = 0;
  portEXIT_CRITICAL(&encMux);
  return (int8_t)constrain((int)v, -100, 100);
}

bool encoderButtonPressed(RotaryEncoder &enc) {
  bool now = digitalRead(enc.pinKey);

  if (now != enc.lastKey && millis() - enc.lastKeyChange >= 30) {
    enc.lastKeyChange = millis();
    enc.lastKey = now;

    if (now == LOW) return true;
  }

  return false;
}

// Kafelek podswietlany po wejsciu do menu z ekranu odtwarzacza: ten, ktory odpowiada
// aktualnemu zrodlu (Radio FM / Internet / Karta SD). Po powrocie z podmenu (Wstecz)
// zostaje kafelek, z ktorego wyszlismy (lastMainTile).
int tileForCurrentSource() {
  int mid = (currentSource == SOURCE_RADIO) ? 2 : (currentSource == SOURCE_WEB) ? 6 :
            (currentSource == SOURCE_SD) ? 1 : -1;
  if (mid < 0) return lastMainTile;
  for (int i = 0; i < mainMenuCount; i++) if (mainMenuOrder[i] == mid) return i;
  return lastMainTile;
}

// Przycisk "wybierz" (ENC2 / BTN4). Na pliku MP3 w liscie SD krotkie nacisniecie wybiera utwor
// dopiero po PUSZCZENIU przycisku, a przytrzymanie 0,8 s otwiera potwierdzenie skasowania.
// W pozostalych miejscach reakcja jest natychmiastowa. Zwraca true = wykonaj "wybierz".
struct LongPressSel { bool pending; uint32_t at; int idx; uint8_t pin; };

bool sdFileSelected() {
  return currentUi == UI_MENU_SD && sdMenuEntryCount > 0 && menuIndex < sdMenuEntryCount &&
         !sdMenuEntries[menuIndex].isDirectory;
}

bool okKeyEvent(LongPressSel &lp, bool edge) {
  if (lp.pending) {
    bool down = (digitalRead(lp.pin) == LOW);
    uint32_t held = millis() - lp.at;
    if (!down && held >= 30) {
      lp.pending = false;
      return sdFileSelected() && menuIndex == lp.idx;   // krotkie nacisniecie
    }
    if (down && held >= 800) {
      lp.pending = false;
      if (sdFileSelected() && menuIndex == lp.idx) startDeleteConfirm();
    }
    return false;
  }
  if (!edge) return false;
  if (sdFileSelected()) {
    lp.pending = true;
    lp.at = millis();
    lp.idx = menuIndex;
    return false;
  }
  return true;
}

void showVolumeOverlay() {
  volumeOverlayValue = activeVolumePercent();
  volumeOverlayMax = activeVolumeMax();
  volumeOverlayVisible = true;
  volumeOverlayUntil = millis() + VOLUME_OSD_TIME_MS;
  oledUpdate(true);
}

void sortFmStations() {
  if (fmStationCount <= 1) return;

  for (int i = 0; i < fmStationCount - 1; ++i) {
    for (int j = i + 1; j < fmStationCount; ++j) {
      if (fmStations[j] < fmStations[i]) {
        float tmp = fmStations[i];
        fmStations[i] = fmStations[j];
        fmStations[j] = tmp;
        String name = fmStationNames[i];
        fmStationNames[i] = fmStationNames[j];
        fmStationNames[j] = name;
      }
    }
  }

  if (currentStationIndex >= fmStationCount) {
    currentStationIndex = fmStationCount - 1;
  }
}

void handleEncoders() {
  // ---------------- Enkoder 1 = GLOSNOSC / PAUZA / MUTE ----------------
  // Podczas odtwarzania MP3 (ekran odtwarzacza) zamienione sa tylko PRZYCISKI
  // enkoderow: ENC1 = MENU, ENC2 = PAUZA. Obroty dzialaja jak zwykle.
  int8_t rawStep1 = encoderRead(enc1);
  int8_t rawStep2 = encoderRead(enc2);
  bool key1 = encoderButtonPressed(enc1);
  bool key2 = encoderButtonPressed(enc2);
  bool swapped = false;   // przyciski enkoderow maja te same funkcje dla kazdego zrodla

  int8_t volStep = rawStep1;
  if (volStep != 0) {
    if (currentUi == UI_TEXT_ENTRY) {
      for (int k = 0; k < abs(volStep); k++) entryChangeSet(volStep > 0 ? 1 : -1);
    } else if (currentUi == UI_MENU_SOUND) {
      for (int k = 0; k < abs(volStep); k++) soundAdjust(volStep > 0 ? 1 : -1);   // ENC1: zmiana wartosci zaznaczonej pozycji korektora
    } else if (currentUi == UI_MENU_DATETIME) {
      for (int k = 0; k < abs(volStep); k++) datetimeEditField = (datetimeEditField + (volStep > 0 ? 1 : 4)) % 5;
      dateTimeSaved = false;
      oledUpdate(true);
    } else {
      if (dacIsActiveSource()) {
        // Powolny obrot = 1 krok, szybki obrot = wiecej (przyspieszenie), zeby byl i precyzyjnie, i szybko.
        static uint32_t lastVolTurnMs = 0;
        uint32_t nowMs = millis();
        uint32_t gap = (nowMs - lastVolTurnMs) / abs(volStep);   // kilka zebranych krokow = szybki obrot
        lastVolTurnMs = nowMs;
        int mult = gap < 30 ? 4 : gap < 70 ? 3 : gap < 140 ? 2 : 1;
        dacVolRef() = constrain(dacVolRef() + volStep * mult, 0, DAC_VOLUME_UI_MAX);
      } else volumeLevel = constrain(volumeLevel + volStep, 0, 15);
      applyVolume();
      audioSettingsDirtyAt = millis() ? millis() : 1;
      showVolumeOverlay();
    }
  }

  if (swapped ? key2 : key1) {
    if (currentUi == UI_TEXT_ENTRY) {
      // Wpisywanie tekstu: przycisk ENC1 = usun ostatni znak
      entryBackspace();
    } else if (currentUi != UI_PLAYER) {
      // W menu przycisk ENC1 = COFNIJ
      menuBack();
    } else if (currentSource == SOURCE_WEB) {
      // Radio internetowe = MUTE / UNMUTE
      webMuted = !webMuted;
      applyVolume();
      Serial.println(webMuted ? F("MUTE ON") : F("MUTE OFF"));
      oledUpdate(true);
    } else if (currentSource == SOURCE_SD) {
      // Podczas odtwarzania MP3 = PAUZA / WZNOW
      sdPaused = !sdPaused;
      audio.pauseResume();
      Serial.println(sdPaused ? F("PAUZA") : F("WZNOWIONO"));
      oledUpdate(true);
    } else {
      // Podczas radia = MUTE / UNMUTE
      radioMuted = !radioMuted;
      if (radioReady) si4703SetMute(radioMuted);
      Serial.println(radioMuted ? F("MUTE ON") : F("MUTE OFF"));
      oledUpdate(true);
    }
  }

  // ---------------- Enkoder 2 = MENU / RECZNE STROJENIE ----------------
  int8_t step2 = rawStep2;
  if (step2 != 0) {
    if (currentUi != UI_PLAYER) {
      if (currentUi == UI_MENU_DATETIME) {
        adjustDateTimeField(step2);
      } else if (currentUi == UI_TEXT_ENTRY) {
        for (int k = 0; k < abs(step2); k++) entryChangeChar(step2 > 0 ? 1 : -1);
      } else {
        // W menu: przewijanie listy (SD, stacje, menu glowne)
        for (int k = 0; k < abs(step2); k++) { if (step2 > 0) menuDown(); else menuUp(); }
      }
    } else if (currentSource == SOURCE_RADIO) {
      // W odtwarzaczu radio: enkoder zmienia nastepną zapisaną stację,
      // a przyciski BTN1/BTN2 robia skok czestotliwosci.
      for (int k = 0; k < abs(step2); k++) stationStep(step2 > 0 ? 1 : -1);
    } else if (currentSource == SOURCE_WEB) {
      // Radio internetowe: enkoder zmienia stacje
      for (int k = 0; k < abs(step2); k++) webStationStep(step2 > 0 ? 1 : -1);
    } else if (currentSource == SOURCE_SD) {
      // Podczas odtwarzania MP3 enkoder wybiera poprzedni/nastepny utwor.
      if (step2 > 0) playNextTrack(); else playPrevTrack();
    }
  }

  // Przycisk enkodera 2 = wejscie do menu / wybor pozycji.
  static LongPressSel lpEnc2 = { false, 0, 0, ENC2_KEY };
  if (okKeyEvent(lpEnc2, swapped ? key1 : key2)) {
    if (currentUi == UI_MENU_DATETIME) {
      saveDateTimeEdit();
    } else if (currentUi == UI_PLAYER) {
      currentUi = UI_MENU_MAIN;
      menuIndex = tileForCurrentSource();
      menuScroll = 0;
    } else {
      menuEnter();
    }
    oledUpdate(true);
  }
}

// ================== OBSLUGA PRZYCISKOW (MODUL 4-KEY) ==================

void buttonsInit() {
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN3_PIN, INPUT_PULLUP);
  pinMode(BTN4_PIN, INPUT_PULLUP);

  btn1.lastState = digitalRead(BTN1_PIN);
  btn2.lastState = digitalRead(BTN2_PIN);
  btn3.lastState = digitalRead(BTN3_PIN);
  btn4.lastState = digitalRead(BTN4_PIN);
}

bool buttonPressed(PhysButton &b) {
  bool now = digitalRead(b.pin);

  if (now != b.lastState && millis() - b.lastChange >= 30) {
    b.lastChange = millis();
    b.lastState = now;

    if (now == LOW) return true; // modul aktywny stanem LOW ("Press output low")
  }

  return false;
}

bool buttonHeld(PhysButton &b, uint32_t holdMs) {
  bool nowPressed = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();

  if (!nowPressed) {
    b.lastHoldTrigger = 0;
    return false;
  }

  if (b.lastChange == 0) {
    b.lastChange = now;
  }

  if (now - b.lastChange >= holdMs &&
      (b.lastHoldTrigger == 0 || now - b.lastHoldTrigger >= 100)) {
    b.lastHoldTrigger = now;
    return true;
  }

  return false;
}

void stationStep(int8_t dir) {
  if (fmStationCount == 0) {
    Serial.println(F("Brak zeskanowanych stacji - uzyj 'scan' lub Menu -> 4."));
    return;
  }

  if (currentStationIndex < 0) {
    currentStationIndex = findClosestStationIndex(radioFrequencyKHz);
  }
  currentStationIndex = (currentStationIndex + dir + fmStationCount) % fmStationCount;

  setRadioFrequencyKHz((uint32_t)(fmStations[currentStationIndex] * 1000.0f + 0.5f));
  radioMuted = false;
  if (radioReady) si4703SetMute(false);
  oledUpdate(true);
}

void stepRadioFrequencyByButton(int8_t dir) {
  if (currentSource != SOURCE_RADIO || currentUi != UI_PLAYER) return;

  int32_t newFreqKHz = (int32_t)radioFrequencyKHz + (int32_t)dir * (int32_t)RADIO_STEP_KHZ;
  if (newFreqKHz < RADIO_MIN_KHZ) newFreqKHz = RADIO_MIN_KHZ;
  if (newFreqKHz > RADIO_MAX_KHZ) newFreqKHz = RADIO_MAX_KHZ;

  setRadioFrequencyKHz((uint32_t)newFreqKHz);
  currentStationIndex = -1;
  radioMuted = false;
  if (radioReady) si4703SetMute(false);
  oledUpdate(true);
}

void seekAudioSeconds(int seconds) {
  if (currentSource != SOURCE_SD || !audioInitialized) {
    return;
  }

  int32_t target = (int32_t)audio.getAudioCurrentTime() + seconds;
  uint32_t duration = audio.getAudioFileDuration();
  if (target < 0) target = 0;
  if (duration > 0 && (uint32_t)target >= duration) target = duration - 1;

  if (audio.setAudioPlayPosition((uint16_t)target)) {
    Serial.printf("Pozycja MP3: %ld s\n", (long)target);
  } else {
    Serial.println(F("BLAD: Nie mozna przewinac tego pliku MP3."));
  }
  oledUpdate(true);
}

void seekAudioSmooth(int seconds) {
  if (currentSource != SOURCE_SD || !audioInitialized) return;
  if (!audio.setTimeOffset(seconds)) return;
  oledUpdate(true);
}

void changePlaybackSpeed(int direction) {
  if (currentSource != SOURCE_SD || !audioInitialized) return;

  int currentIndex = 1;
  for (int i = 0; i < playbackSpeedCount; i++) {
    if (fabsf(playbackSpeeds[i] - playbackSpeed) < 0.01f) {
      currentIndex = i;
      break;
    }
  }

  int nextIndex = constrain(currentIndex + direction, 0, playbackSpeedCount - 1);
  float newSpeed = playbackSpeeds[nextIndex];

  if (audio.audioFileSeek(newSpeed)) {
    playbackSpeed = newSpeed;
    Serial.printf("Predkosc MP3: %.1fx\n", playbackSpeed);
    oledUpdate(true);
  }
}

void handleButtons() {
  // --- BTN1 / BTN2: menu/radio oraz predkosc MP3 ---
  if (buttonPressed(btn1)) {
    if (currentUi != UI_PLAYER && currentUi != UI_MENU_DATETIME) {
      menuUp();
    } else if (currentSource == SOURCE_RADIO) {
      stepRadioFrequencyByButton(-1);
    } else if (currentSource == SOURCE_SD) {
      changePlaybackSpeed(1);
    } else if (currentSource == SOURCE_WEB) {
      webStationStep(-1);
    }
  }

  if (buttonPressed(btn2)) {
    if (currentUi != UI_PLAYER && currentUi != UI_MENU_DATETIME) {
      menuDown();
    } else if (currentSource == SOURCE_RADIO) {
      stepRadioFrequencyByButton(1);
    } else if (currentSource == SOURCE_SD) {
      changePlaybackSpeed(-1);
    } else if (currentSource == SOURCE_WEB) {
      webStationStep(1);
    }
  }

  // --- BTN3: w odtwarzaczu = ZRODLO (Radio<->SD), w menu = COFNIJ ---
  if (buttonPressed(btn3)) {
    if (currentUi == UI_MENU_DATETIME) {
      adjustDateTimeField(1);
    } else if (currentUi != UI_PLAYER) {
      menuBack();
    } else {
      toggleSource();
    }
  }

  // --- BTN4: w odtwarzaczu = wejdz do MENU, w menu = WYBIERZ (OK) ---
  static LongPressSel lpBtn4 = { false, 0, 0, BTN4_PIN };
  if (okKeyEvent(lpBtn4, buttonPressed(btn4))) {
    if (currentUi == UI_MENU_DATETIME) {
      adjustDateTimeField(-1);
    } else if (currentUi == UI_PLAYER && currentSource == SOURCE_WEB) {
      recorderToggle();   // radio internetowe: BTN4 = start / stop nagrywania
    } else if (currentUi == UI_PLAYER) {
      currentUi = UI_MENU_MAIN;
      menuIndex = tileForCurrentSource();
      menuScroll = 0;
      oledUpdate(true);
    } else {
      menuEnter();
    }
  }
}

// ================== WI-FI I RADIO INTERNETOWE ==================

#define WEB_STATION_COUNT 0  // musi odpowiadac liczbie wpisow w webStations[]
String uiMsg1, uiMsg2;
uint32_t uiMessageUntil = 0;

void showUiMessage(const String &line1, const String &line2 = "", uint32_t ms = 2000) {
  uiMsg1 = line1;
  uiMsg2 = line2;
  uiMessageUntil = millis() + ms;
  oledUpdate(true);
}

// Prosty ekran "prosze czekac" rysowany bezposrednio (do operacji blokujacych).
void drawBusyScreen(const char *line1, const String &line2) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB10_te);
  if (oled.getUTF8Width(line1) > 124) oled.setFont(u8g2_font_6x12_te);
  int w = oled.getUTF8Width(line1);
  oled.drawUTF8((128 - w) / 2, 28, line1);

  oled.setFont(u8g2_font_NokiaSmallPlain_te);   // nazwa sieci / stacji nieco mniejsza
  String second = line2;
  while (second.length() > 3 && oled.getUTF8Width(second.c_str()) > 120) removeLastUtf8Char(second);
  w = oled.getUTF8Width(second.c_str());
  oled.drawUTF8((128 - w) / 2, 46, second.c_str());

  int dots = (millis() / 300) % 4;
  for (int i = 0; i < dots; i++) oled.drawDisc(54 + i * 7, 58, 1);
  oledSend();
}

// --- Dane logowania do Wi-Fi w pamieci nieulotnej (NVS) ---
Preferences prefs;

// Haslo Wi-Fi jest zapisywane zaszyfrowane (AES-256-GCM). Klucz powstaje z unikalnego numeru
// ukladu (eFuse MAC) i stalej soli, wiec zrzut pamieci flash z jednego radia nie odszyfruje sie
// na innym egzemplarzu. Format w NVS ("passe"): IV(12) | TAG(16) | szyfrogram.
static void wifiDeriveKey(uint8_t key[32]) {
  uint64_t mac = ESP.getEfuseMac();
  static const char salt[] = "";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const uint8_t*)&mac, sizeof(mac));
  mbedtls_sha256_update(&ctx, (const uint8_t*)salt, sizeof(salt) - 1);
  mbedtls_sha256_finish(&ctx, key);
  mbedtls_sha256_free(&ctx);
}

static bool wifiPassEncrypt(const String &plain, uint8_t *out, size_t &outLen) {
  size_t n = plain.length();
  if (n == 0 || n > 64) return false;
  uint8_t key[32];
  wifiDeriveKey(key);
  uint8_t *iv = out, *tag = out + 12, *ct = out + 28;
  esp_fill_random(iv, 12);   // sprzetowy generator liczb losowych
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
            mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, iv, 12, nullptr, 0,
                                      (const uint8_t*)plain.c_str(), ct, 16, tag) == 0;
  mbedtls_gcm_free(&gcm);
  memset(key, 0, sizeof(key));
  outLen = 28 + n;
  return ok;
}

static bool wifiPassDecrypt(const uint8_t *in, size_t inLen, String &plain) {
  plain = "";
  if (inLen <= 28 || inLen > 28 + 64) return false;
  size_t n = inLen - 28;
  uint8_t key[32], buf[65];
  wifiDeriveKey(key);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
            mbedtls_gcm_auth_decrypt(&gcm, n, in, 12, nullptr, 0, in + 12, 16, in + 28, buf) == 0;
  mbedtls_gcm_free(&gcm);
  memset(key, 0, sizeof(key));
  if (ok) { buf[n] = 0; plain = String((const char*)buf); }
  memset(buf, 0, sizeof(buf));
  return ok;
}

void wifiLoadCredentials() {
  prefs.begin("radio", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = "";
  uint8_t blob[28 + 64];
  size_t len = prefs.getBytesLength("passe");
  if (len > 0 && len <= sizeof(blob)) {
    prefs.getBytes("passe", blob, len);
    wifiPassDecrypt(blob, len, wifiPass);
  } else if (prefs.isKey("pass")) {
    // Migracja ze starej wersji: haslo bylo zapisane jawnie - szyfrujemy i kasujemy tekst
    wifiPass = prefs.getString("pass", "");
    size_t outLen = 0;
    if (wifiPassEncrypt(wifiPass, blob, outLen)) {
      prefs.putBytes("passe", blob, outLen);
      prefs.remove("pass");
    }
  }
  prefs.end();
  memset(blob, 0, sizeof(blob));
}

void wifiSaveCredentials(const String &ssid, const String &pass) {
  prefs.begin("radio", false);
  prefs.putString("ssid", ssid);
  uint8_t blob[28 + 64];
  size_t outLen = 0;
  if (wifiPassEncrypt(pass, blob, outLen)) prefs.putBytes("passe", blob, outLen);
  else prefs.remove("passe");   // siec otwarta (bez hasla)
  prefs.remove("pass");
  prefs.end();
  memset(blob, 0, sizeof(blob));
  wifiSsid = ssid;
  wifiPass = pass;
}

void wifiForgetCredentials() {
  prefs.begin("radio", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.remove("passe");
  prefs.end();
  wifiSsid = "";
  wifiPass = "";
}

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

void wifiOff() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  batteryRefreshRequested = true;
}

bool wifiConnect(const String &ssid, const String &pass, uint32_t timeoutMs = 15000) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);   // bez usypiania modemu - stabilniejszy i szybszy stream
  WiFi.begin(ssid.c_str(), pass.length() > 0 ? pass.c_str() : nullptr);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    drawBusyScreen("Łączenie z Wi-Fi", ssid);
    if (WiFi.status() == WL_CONNECT_FAILED && millis() - t0 > 2000) break;
    delay(150);
  }
  return WiFi.status() == WL_CONNECTED;
}

// --- Skanowanie sieci ---
struct WifiNet {
  String ssid;
  int rssi;
  bool open;
};
WifiNet wifiNets[12];
int wifiNetCount = 0;

void wifiScan() {
  if (currentSource == SOURCE_WEB) stopPlayback();   // wylacza Wi-Fi
  drawBusyScreen("Szukam sieci", "");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  wifiNetCount = 0;
  for (int i = 0; i < n && wifiNetCount < 12; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < wifiNetCount; j++) {
      if (wifiNets[j].ssid == ssid) { dup = true; break; }
    }
    if (dup) continue;
    wifiNets[wifiNetCount].ssid = ssid;
    wifiNets[wifiNetCount].rssi = WiFi.RSSI(i);
    wifiNets[wifiNetCount].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    wifiNetCount++;
  }
  WiFi.scanDelete();
  wifiOff();

  // Najsilniejsze sieci na gorze listy.
  for (int i = 0; i < wifiNetCount - 1; i++) {
    for (int j = i + 1; j < wifiNetCount; j++) {
      if (wifiNets[j].rssi > wifiNets[i].rssi) {
        WifiNet tmp = wifiNets[i];
        wifiNets[i] = wifiNets[j];
        wifiNets[j] = tmp;
      }
    }
  }
}

// --- Wpisywanie tekstu enkoderami (SSID / haslo) ---
enum EntryPurpose { ENTRY_SSID, ENTRY_PASS };
EntryPurpose entryPurpose = ENTRY_PASS;
String entryText;
String pendingSsid;
int entrySet = 0;
int entryIdx = 0;

const char* const entrySetLabels[] = { "a-z", "A-Z", "0-9", "#@!", "AKCJE" };
const int entrySetCount = 5;
const char* const entrySetChars[] = {
  "abcdefghijklmnopqrstuvwxyz",
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  "0123456789",
  " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
  ""
};

int entrySetLength(int set) {
  return set == 4 ? 2 : (int)strlen(entrySetChars[set]);
}

String entryLabel(int set, int idx) {
  if (set == 4) return idx == 0 ? "<-" : "OK";
  char c = entrySetChars[set][idx];
  if (c == ' ') return "\xC2\xB7";   // spacja pokazana jako kropka
  return String(c);
}

void startTextEntry(EntryPurpose purpose, const String &initial = "") {
  entryPurpose = purpose;
  entryText = initial;
  entrySet = 0;
  entryIdx = 0;
  currentUi = UI_TEXT_ENTRY;
  oledUpdate(true);
}

void entryChangeChar(int d) {
  int n = entrySetLength(entrySet);
  entryIdx = (entryIdx + d + n) % n;
  oledUpdate(true);
}

void entryChangeSet(int d) {
  entrySet = (entrySet + d + entrySetCount) % entrySetCount;
  entryIdx = 0;
  oledUpdate(true);
}

void entryBackspace() {
  if (entryText.length() > 0) entryText.remove(entryText.length() - 1);
  oledUpdate(true);
}

void wifiConnectAndSave(const String &ssid, const String &pass) {
  if (currentSource == SOURCE_WEB) stopPlayback();   // wylacza Wi-Fi przed nowym laczeniem
  bool ok = wifiConnect(ssid, pass);
  if (ok) {
    wifiSaveCredentials(ssid, pass);
    wifiOff();   // Wi-Fi wlaczy sie samo przy starcie radia internetowego
    showUiMessage("Połączono", ssid, 2500);
  } else {
    wifiOff();
    showUiMessage("Błąd połączenia", "Sprawdź hasło/zasięg", 2500);
  }
  currentUi = UI_MENU_WIFI;
  menuIndex = 0;
  menuScroll = 0;
  oledUpdate(true);
}

void entryFinish() {
  if (entryPurpose == ENTRY_SSID) {
    if (entryText.length() == 0) {
      showUiMessage("Podaj nazwę sieci", "", 1500);
      return;
    }
    pendingSsid = entryText;
    startTextEntry(ENTRY_PASS, "");
  } else {
    wifiConnectAndSave(pendingSsid, entryText);
  }
}

void entryInsert() {
  if (entrySet == 4) {
    if (entryIdx == 0) entryBackspace();
    else entryFinish();
    return;
  }
  size_t maxLen = (entryPurpose == ENTRY_SSID) ? 32 : 63;
  if (entryText.length() < maxLen) entryText += String(entrySetChars[entrySet][entryIdx]);
  oledUpdate(true);
}

void entryCancel() {
  currentUi = UI_MENU_WIFI;
  menuIndex = 0;
  menuScroll = 0;
  oledUpdate(true);
}

// --- Menu: Ustawienia i Wi-Fi ---
const char* settingsItems[] = { "Wi-Fi", "Dźwięk", "Wyświetlacz", "Zegar", "Wyczyść kartę SD", "Informacje" };
const int settingsItemCount = 6;

const char* displayModeItems[] = { "Tryb ciemny", "Tryb jasny" };
const int displayModeCount = 2;

void displayLoadSettings() {
  prefs.begin("radio", true);
  lightMode = prefs.getBool("light", false);
  prefs.end();
}

void displaySaveSettings() {
  prefs.begin("radio", false);
  prefs.putBool("light", lightMode);
  prefs.end();
}

// --- Glosnosc (FM / SD / WWW osobno) i korektor w pamieci nieulotnej ---
void audioSettingsLoad() {
  prefs.begin("radio", true);
  volumeLevel = constrain(prefs.getInt("vFm", volumeLevel), 0, 15);
  dacVolumeSd = constrain(prefs.getInt("vSd", dacVolumeSd), 0, DAC_VOLUME_UI_MAX);
  dacVolumeWeb = constrain(prefs.getInt("vWeb", dacVolumeWeb), 0, DAC_VOLUME_UI_MAX);
  eqLow = constrain(prefs.getInt("eqL", 0), EQ_MIN, EQ_MAX);
  eqMid = constrain(prefs.getInt("eqM", 0), EQ_MIN, EQ_MAX);
  eqHigh = constrain(prefs.getInt("eqH", 0), EQ_MIN, EQ_MAX);
  eqCustom[0] = constrain(prefs.getInt("cL", eqLow), EQ_MIN, EQ_MAX);
  eqCustom[1] = constrain(prefs.getInt("cM", eqMid), EQ_MIN, EQ_MAX);
  eqCustom[2] = constrain(prefs.getInt("cH", eqHigh), EQ_MIN, EQ_MAX);
  eqPreset = constrain(prefs.getInt("eqP", -1), -1, eqPresetCount - 1);
  eqBalance = constrain(prefs.getInt("bal", 0), -16, 16);
  eqLoudness = prefs.getBool("loud", false);
  lastMainTile = prefs.getInt("tile", 0);
  startFmKHz = prefs.getUInt("fmKHz", 0);
  webStationIndex = prefs.getInt("webIdx", 0);
  startSource = prefs.getInt("src", 2);
  prefs.end();
  savedFmKHz = startFmKHz; savedWebIdx = webStationIndex; savedTile = lastMainTile; savedSrc = startSource;
}

void audioSettingsSave() {
  prefs.begin("radio", false);
  prefs.putInt("vFm", volumeLevel);
  prefs.putInt("vSd", dacVolumeSd);
  prefs.putInt("vWeb", dacVolumeWeb);
  prefs.putInt("eqL", eqLow);
  prefs.putInt("eqM", eqMid);
  prefs.putInt("eqH", eqHigh);
  prefs.putInt("cL", eqCustom[0]);
  prefs.putInt("cM", eqCustom[1]);
  prefs.putInt("cH", eqCustom[2]);
  prefs.putInt("eqP", eqPreset);
  prefs.putInt("bal", eqBalance);
  prefs.putBool("loud", eqLoudness);
  int srcNow = (currentSource == SOURCE_WEB) ? 3 : (currentSource == SOURCE_RADIO) ? 2 : (currentSource == SOURCE_SD) ? 1 : savedSrc;
  prefs.putInt("tile", lastMainTile);
  prefs.putUInt("fmKHz", radioFrequencyKHz);
  prefs.putInt("webIdx", webStationIndex);
  prefs.putInt("src", srcNow);
  prefs.end();
  savedFmKHz = radioFrequencyKHz; savedWebIdx = webStationIndex; savedTile = lastMainTile; savedSrc = srcNow;
}

// Wywolywane w petli: zapisuje po 2 s od ostatniej zmiany (zeby nie zuzywac pamieci flash).
void audioSettingsService() {
  // Zmiana stacji FM / WWW, zrodla albo kafelka menu tez uruchamia zapis (po 2 s spokoju).
  int srcNow = (currentSource == SOURCE_WEB) ? 3 : (currentSource == SOURCE_RADIO) ? 2 : (currentSource == SOURCE_SD) ? 1 : -1;
  if (audioSettingsDirtyAt == 0 &&
      ((srcNow >= 0 && srcNow != savedSrc) || radioFrequencyKHz != savedFmKHz ||
       webStationIndex != savedWebIdx || lastMainTile != savedTile)) {
    audioSettingsDirtyAt = millis() ? millis() : 1;
  }
  if (audioSettingsDirtyAt != 0 && millis() - audioSettingsDirtyAt > 2000) {
    audioSettingsDirtyAt = 0;
    audioSettingsSave();
  }
}

// --- Korektor ---
void soundMarkChanged() {
  eqApply();
  audioSettingsDirtyAt = millis() ? millis() : 1;
  oledUpdate(true);
}

String soundItemText(int i) {
  char buf[32];
  switch (i) {
    case 0:
      return String("Preset: ") + (eqPreset < 0 ? "Własny" : eqPresets[eqPreset].name);
    case 1: case 2: case 3: {
      const char* names[] = { "Niskie", "Średnie", "Wysokie" };
      int v = (i == 1) ? eqLow : (i == 2) ? eqMid : eqHigh;
      snprintf(buf, sizeof(buf), "%s: %+d dB", names[i - 1], v);
      return String(buf);
    }
    case 4:
      if (eqBalance == 0) return "Balans: środek";
      snprintf(buf, sizeof(buf), "Balans: %c%d", eqBalance < 0 ? 'L' : 'P', abs(eqBalance));
      return String(buf);
    case 5:
      return String("Loudness: ") + (eqLoudness ? "wł." : "wył.");
    default:
      return "Wyzeruj";
  }
}

void eqSetPreset(int p) {
  eqPreset = p;
  if (p >= 0) {
    eqLow = eqPresets[p].low; eqMid = eqPresets[p].mid; eqHigh = eqPresets[p].high;
  } else {
    eqLow = eqCustom[0]; eqMid = eqCustom[1]; eqHigh = eqCustom[2];
  }
}

// Obrot ENC1 na zaznaczonej pozycji: preset / pasmo / balans / loudness.
void soundAdjust(int d) {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  int mult = (now - lastMs < 60) ? 3 : 1;   // szybki obrot = wiekszy krok (zakres pasm to 46 dB)
  lastMs = now;

  if (menuIndex == 0) {
    int p = eqPreset + d;
    if (p < -1) p = eqPresetCount - 1;
    if (p >= eqPresetCount) p = -1;
    eqSetPreset(p);
  } else if (menuIndex >= 1 && menuIndex <= 3) {
    int8_t *target = (menuIndex == 1) ? &eqLow : (menuIndex == 2) ? &eqMid : &eqHigh;
    *target = constrain(*target + d * mult, EQ_MIN, EQ_MAX);
    eqPreset = -1;   // reczna zmiana = ustawienia wlasne
    eqCustom[0] = eqLow; eqCustom[1] = eqMid; eqCustom[2] = eqHigh;
  } else if (menuIndex == 4) {
    eqBalance = constrain(eqBalance + d, -16, 16);
  } else if (menuIndex == 5) {
    eqLoudness = !eqLoudness;
  } else {
    return;
  }
  soundMarkChanged();
}

// Przycisk ENC2 / BTN: preset -> nastepny, loudness -> przelacz, "Wyzeruj" -> reset.
void soundEnter(int idx) {
  if (idx == 0) { soundAdjust(1); return; }
  if (idx == 5) { soundAdjust(1); return; }
  if (idx == 6) {
    eqPreset = -1;
    eqLow = eqMid = eqHigh = 0;
    eqCustom[0] = eqCustom[1] = eqCustom[2] = 0;
    eqBalance = 0;
    eqLoudness = false;
    soundMarkChanged();
  }
}

const char* wifiMenuItems[] = { "Szukaj sieci", "Dodaj ręcznie", "Połącz", "Rozłącz", "Zapomnij sieć" };
const int wifiMenuItemCount = 5;

void wifiMenuEnter(int idx) {
  if (idx == 0) {
    wifiScan();
    if (wifiNetCount == 0) {
      showUiMessage("Nie znaleziono", "żadnych sieci", 2000);
    } else {
      currentUi = UI_WIFI_SCAN;
      menuIndex = 0;
      menuScroll = 0;
    }
  } else if (idx == 1) {
    startTextEntry(ENTRY_SSID, "");
  } else if (idx == 2) {
    if (wifiSsid.length() == 0) showUiMessage("Brak zapisanej sieci", "", 2000);
    else wifiConnectAndSave(wifiSsid, wifiPass);
  } else if (idx == 3) {
    if (currentSource == SOURCE_WEB) webReturnToFm();
    wifiOff();
    showUiMessage("Wi-Fi wyłączone", "", 1500);
  } else if (idx == 4) {
    if (currentSource == SOURCE_WEB) webReturnToFm();
    wifiForgetCredentials();
    wifiOff();
    showUiMessage("Sieć zapomniana", "", 1500);
  }
}

void wifiScanEnter(int idx) {
  if (idx < 0 || idx >= wifiNetCount) return;
  pendingSsid = wifiNets[idx].ssid;
  if (wifiNets[idx].open) wifiConnectAndSave(pendingSsid, "");
  else startTextEntry(ENTRY_PASS, "");
}

// Liczba widocznych pozycji: lista z podtytulem (Wi-Fi) ma o jeden wiersz mniej.
int listVisibleRows() { return currentUi == UI_MENU_WIFI ? 3 : itemsPerPage; }

int listItemCount() {
  switch (currentUi) {
    case UI_MENU_MAIN:     return mainMenuCount;
    case UI_MENU_SD:       return sdMenuEntryCount;
    case UI_MENU_RADIO:    return fmStationCount;
    case UI_MENU_WEB:      return WEB_STATION_COUNT;
    case UI_MENU_SETTINGS: return settingsItemCount;
    case UI_MENU_DISPLAY:  return displayModeCount;
    case UI_MENU_SOUND:    return SOUND_ITEM_COUNT;
    case UI_MENU_WIFI:     return wifiMenuItemCount;
    case UI_WIFI_SCAN:     return wifiNetCount;
    default:               return fmStationCount;
  }
}

// --- Radio internetowe ---
struct WebStation {
  const char* name;
  const char* url;
};
const WebStation webStations[] = {

};
const int webStationCount = sizeof(webStations) / sizeof(webStations[0]);
static_assert(sizeof(webStations) / sizeof(webStations[0]) == WEB_STATION_COUNT, "WEB_STATION_COUNT != liczba stacji");

// Zostawia tylko poprawne sekwencje UTF-8 (strumienie czesto nadaja tytuly w innym kodowaniu).
String sanitizeUtf8(const char *text) {
  String out;
  const uint8_t *p = (const uint8_t *)text;
  while (*p) {
    if (*p < 0x80) {
      out += (char)*p;
      p++;
      continue;
    }
    int len = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : (*p >= 0xC0) ? 2 : 0;
    bool ok = len > 0;
    for (int i = 1; i < len && ok; i++) {
      if ((p[i] & 0xC0) != 0x80) ok = false;
    }
    if (ok) {
      for (int i = 0; i < len; i++) out += (char)p[i];
      p += len;
    } else {
      out += '?';
      p++;
    }
  }
  return out;
}

// Tytul utworu ze strumienia (naglowki ICY) - wywolywane przez biblioteke Audio.
void audio_showstreamtitle(const char *info) {
  if (currentSource != SOURCE_WEB) return;
  webTitle = sanitizeUtf8(info);
  webTitle.trim();
  Serial.printf("Tytul: %s\n", webTitle.c_str());
}

// Koniec / zerwanie strumienia - ponowimy polaczenie w webService().
void audio_eof_stream(const char *info) {
  Serial.printf("Koniec strumienia: %s\n", info);
  if (currentSource == SOURCE_WEB && webReconnectAt == 0) {
    webReconnectAt = millis() + 1500;
  }
}

void webReturnToFm() {
  stopPlayback();   // wylacza Wi-Fi, jesli gralo radio internetowe
  currentSource = SOURCE_RADIO;
  radioMuted = false;
  if (radioReady) si4703SetMute(false);
  digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
  oledUpdate(true);
}

// ================== NAGRYWANIE STRUMIENIA WWW NA KARTE SD ==================
// Osobne polaczenie HTTP do tej samej stacji (bez metadanych ICY) - surowy strumien mp3/aac
// zapisujemy do pliku w /Nagrania. Nie dziala dla stacji HLS (.m3u8).

WiFiClient recPlain;
WiFiClientSecure *recSecure = nullptr;
WiFiClient *recClient = nullptr;
File recFile;
bool recActive = false;
uint32_t recBytes = 0;
uint32_t recStartMs = 0;
uint32_t recLastFlush = 0;
String recPath;

void recCloseClient() {
  if (recClient) recClient->stop();
  recClient = nullptr;
  if (recSecure) { delete recSecure; recSecure = nullptr; }
}

bool recParseUrl(const String &url, bool &ssl, String &host, uint16_t &port, String &path) {
  String u = url;
  if (u.startsWith("https://")) { ssl = true; port = 443; u = u.substring(8); }
  else if (u.startsWith("http://")) { ssl = false; port = 80; u = u.substring(7); }
  else return false;
  int slash = u.indexOf('/');
  String hostPort = slash >= 0 ? u.substring(0, slash) : u;
  path = slash >= 0 ? u.substring(slash) : String("/");
  int colon = hostPort.indexOf(':');
  if (colon >= 0) { host = hostPort.substring(0, colon); port = (uint16_t)hostPort.substring(colon + 1).toInt(); }
  else host = hostPort;
  return host.length() > 0;
}

// Laczy sie ze strumieniem (z obsluga przekierowan), czyta naglowki. Po sukcesie klient stoi na poczatku danych.
bool recOpenStream(const char *url, String &contentType) {
  String cur = url;
  for (int hop = 0; hop < 4; hop++) {
    bool ssl; String host, path; uint16_t port;
    if (!recParseUrl(cur, ssl, host, port, path)) return false;

    recCloseClient();
    if (ssl) {
      recSecure = new WiFiClientSecure();
      recSecure->setInsecure();   // tylko odbior publicznego strumienia radiowego
      recClient = recSecure;
    } else {
      recClient = &recPlain;
    }
    if (!recClient->connect(host.c_str(), port)) return false;

    recClient->print(String("GET ") + path + " HTTP/1.0\r\nHost: " + host +
                     "\r\nUser-Agent: ESP32-Radio\r\nAccept: */*\r\nConnection: close\r\n\r\n");

    // Naglowki odpowiedzi czytamy bajt po bajcie, az do pustej linii
    String headers;
    uint32_t t0 = millis();
    bool headersDone = false;
    while (!headersDone && millis() - t0 < 6000) {
      while (recClient->available()) {
        headers += (char)recClient->read();
        if (headers.endsWith("\r\n\r\n")) { headersDone = true; break; }
        if (headers.length() > 3072) return false;
      }
      if (!headersDone) {
        if (!recClient->connected() && !recClient->available()) break;
        delay(5);
      }
    }
    if (!headersDone) return false;

    int sp = headers.indexOf(' ');
    int code = sp > 0 ? headers.substring(sp + 1, sp + 4).toInt() : 0;
    String lower = headers;
    lower.toLowerCase();

    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      int li = lower.indexOf("\nlocation:");
      if (li < 0) return false;
      int ls = li + 10;
      int le = headers.indexOf('\r', ls);
      String loc = headers.substring(ls, le < 0 ? headers.length() : le);
      loc.trim();
      if (loc.startsWith("/")) loc = String(ssl ? "https://" : "http://") + host + ":" + String(port) + loc;
      cur = loc;
      continue;
    }
    // "ICY 200 OK" (Shoutcast) ma kod 200 jak zwykle HTTP
    if (code != 200) return false;
    if (lower.indexOf("transfer-encoding: chunked") >= 0) return false;   // nieobslugiwane

    int ci = lower.indexOf("\ncontent-type:");
    contentType = "";
    if (ci >= 0) {
      int cs = ci + 14;
      int ce = lower.indexOf('\r', cs);
      contentType = lower.substring(cs, ce < 0 ? lower.length() : ce);
      contentType.trim();
    }
    return true;
  }
  return false;
}

String recSanitizeName(const char *name) {
  String out;
  bool lastUnderscore = false;
  for (const char *p = name; *p; p++) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out += c;
      lastUnderscore = false;
    } else if ((uint8_t)c < 0x80 && !lastUnderscore && out.length() > 0) {
      out += '_';
      lastUnderscore = true;
    }
  }
  while (out.endsWith("_")) out.remove(out.length() - 1);
  return out.length() > 0 ? out : String("radio");
}

void recorderStop(bool announce) {
  if (!recActive) return;
  recActive = false;
  recFile.flush();
  recFile.close();
  recCloseClient();
  Serial.printf("Nagranie zapisane: %s (%u B)\n", recPath.c_str(), (unsigned)recBytes);
  if (announce) {
    int slash = recPath.lastIndexOf('/');
    showUiMessage("Nagranie zapisane", slash >= 0 ? recPath.substring(slash + 1) : recPath, 3000);
  }
}

void recorderStart() {
  if (currentSource != SOURCE_WEB || recActive) return;
  if (!sdReady) { showUiMessage("Brak karty SD", "", 2000); return; }
  if (!wifiIsConnected()) { showUiMessage("Brak Wi-Fi", "", 2000); return; }

  const WebStation &st = webStations[webStationIndex];
  if (strstr(st.url, ".m3u8") != nullptr) {
    showUiMessage("Nie można nagrywać", "stacji HLS", 2500);
    return;
  }

  drawBusyScreen("Start nagrywania", st.name);
  String contentType;
  if (!recOpenStream(st.url, contentType)) {
    recCloseClient();
    showUiMessage("Błąd nagrywania", "brak połączenia", 2500);
    return;
  }

  const char *ext = (contentType.indexOf("aac") >= 0 || contentType.indexOf("mp4") >= 0) ? ".aac" : ".mp3";
  SD.mkdir("/Nagrania");

  char stamp[24];
  if (rtcReady) snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u", rtcYear, rtcMonth, rtcDay, rtcHour, rtcMinute);
  else snprintf(stamp, sizeof(stamp), "%lu", (unsigned long)millis());
  String base = String("/Nagrania/") + recSanitizeName(st.name) + "_" + stamp;
  String path = base + ext;
  for (int n = 2; SD.exists(path.c_str()) && n < 100; n++) path = base + "_" + String(n) + ext;

  recFile = SD.open(path.c_str(), FILE_WRITE);
  if (!recFile) {
    recCloseClient();
    showUiMessage("Błąd zapisu SD", "", 2500);
    return;
  }

  recPath = path;
  recBytes = 0;
  recStartMs = millis();
  recLastFlush = millis();
  recActive = true;
  Serial.printf("Nagrywanie: %s -> %s\n", st.name, recPath.c_str());
  oledUpdate(true);
}

void recorderToggle() {
  if (recActive) recorderStop(true);
  else recorderStart();
  oledUpdate(true);
}

// Wywolywane w petli glownej: przepisuje dane ze strumienia na karte SD.
void recorderService() {
  if (!recActive) return;

  uint8_t buf[1024];
  for (int i = 0; i < 3; i++) {
    int avail = recClient ? recClient->available() : 0;
    if (avail <= 0) break;
    int n = recClient->read(buf, avail < (int)sizeof(buf) ? avail : (int)sizeof(buf));
    if (n <= 0) break;
    if (recFile.write(buf, n) != (size_t)n) {
      recorderStop(false);
      showUiMessage("Błąd zapisu SD", "nagranie przerwane", 3000);
      return;
    }
    recBytes += n;
  }

  if (millis() - recLastFlush > 5000) {
    recFile.flush();
    recLastFlush = millis();
  }

  if (recClient && !recClient->connected() && recClient->available() == 0) {
    recorderStop(false);
    showUiMessage("Nagranie przerwane", "utracono strumień", 3000);
    return;
  }
  if (recBytes > 3900000000UL) recorderStop(true);   // limit FAT32 ~4 GB
}

// Stacja sie nie wczytala: po komunikacie (pokazanym wczesniej przez wywolujacego) przechodzimy na
// NASTEPNA stacje internetowa. Jesli nie dziala zadna - dopiero wtedy wracamy do FM.
void webSkipToNext() {
  webFailStreak++;
  if (webFailStreak >= webStationCount) {
    webFailStreak = 0;
    showUiMessage("Brak działających", "stacji - wracam do FM", 2500);
    webReturnToFm();
    return;
  }
  webPendingIdx = (webStationIndex + 1) % webStationCount;
  webPendingAt = millis() + 2600;   // po zniknieciu komunikatu
  currentUi = UI_PLAYER;
  oledUpdate(true);
}

bool startWebStation(int idx) {
  if (idx < 0 || idx >= webStationCount) return false;
  recorderStop(true);   // zmiana stacji konczy nagrywanie

  if (wifiSsid.length() == 0) {
    showUiMessage("Brak sieci Wi-Fi", "Ustaw > Wi-Fi", 2500);
    return false;
  }

  if (!wifiIsConnected()) {
    if (!wifiConnect(wifiSsid, wifiPass)) {
      if (currentSource != SOURCE_WEB) wifiOff();
      showUiMessage("Brak Wi-Fi", "Sprawdź sieć", 2500);
      return false;
    }
  }

  drawBusyScreen("Łączenie ze stacją", webStations[idx].name);

  if (currentSource == SOURCE_WEB) {
    // Zmiana stacji w trakcie radia: NIE wołamy audio.stopSong() - biblioteka ma osobny
    // watek dekodera, a connecttohost() sama bezpiecznie resetuje stan pod mutexem.
    webTitle = "";
  } else {
    keepWifiOnStop = true;
    stopPlayback();
    keepWifiOnStop = false;
  }
  webPendingIdx = -1;

  if (radioReady) si4703SetMute(true);
  digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_SD);   // wyjscie PCM5102
  ensureAudioInit();

  webTitle = "";
  webMuted = false;
  webStationIndex = idx;
  webReconnectAt = 0;
  webReconnectTries = 0;
  webStartMs = millis();
  currentSource = SOURCE_WEB;
  applyVolume();

  if (!audio.connecttohost(webStations[idx].url)) {
    Serial.printf("BLAD: Nie mozna polaczyc ze stacja %s\n", webStations[idx].name);
    showUiMessage("Błąd stacji", webStations[idx].name, 2500);
    webSkipToNext();
    return false;
  }

  Serial.printf("Radio internetowe: %s\n", webStations[idx].name);
  currentUi = UI_PLAYER;
  oledUpdate(true);
  return true;
}

void webStationStep(int8_t dir) {
  // Kolejne kliki zbieramy i laczymy dopiero po chwili spokoju - kazde laczenie blokuje na sekundy.
  webFailStreak = 0;   // reczna zmiana stacji zaczyna liczenie od nowa
  int base = (webPendingIdx >= 0) ? webPendingIdx : webStationIndex;
  webPendingIdx = (base + dir + webStationCount) % webStationCount;
  webPendingAt = millis() + 450;
  oledUpdate(true);
}

// Wywolywane w petli glownej: ponawianie polaczenia po zerwaniu strumienia.
void webService() {
  if (currentSource != SOURCE_WEB) { webPendingIdx = -1; return; }

  if (webPendingIdx >= 0 && (int32_t)(millis() - webPendingAt) >= 0) {
    int idx = webPendingIdx;
    webPendingIdx = -1;
    if (idx != webStationIndex || webReconnectAt != 0) startWebStation(idx);
    else oledUpdate(true);
    return;
  }

  if (webReconnectAt == 0) {
    if (millis() - webStartMs > 15000) { webReconnectTries = 0; webFailStreak = 0; }   // stacja gra stabilnie
    return;
  }
  if ((int32_t)(millis() - webReconnectAt) < 0) return;
  webReconnectAt = 0;

  if (webReconnectTries >= 3) {
    showUiMessage("Utracono sygnał", "Następna stacja", 2500);
    webSkipToNext();
    return;
  }
  webReconnectTries++;

  if (!wifiIsConnected()) {
    WiFi.reconnect();
    webReconnectAt = millis() + 4000;
    return;
  }

  webStartMs = millis();
  if (!audio.connecttohost(webStations[webStationIndex].url)) {
    webReconnectAt = millis() + 3000;
  }
}


// ================== LOGIKA MENU ==================

void scanSdFiles() {
  sdFileCount = 0;
  File root = SD.open("/");
  if (!root) return;

  if (root.isDirectory()) {
    scanSdFilesRecursive(root, "/");
  }

  root.close();
}

String sdEntryLabel(const String &path) {
  int slash = path.lastIndexOf('/');
  return (slash >= 0) ? path.substring(slash + 1) : path;
}

// --- Dluzsze operacje na karcie SD ---
// Magistrala SPI karty (wspolna z OLED-em) potrafi zaklocac analogowy tor FM - slychac wtedy szum/swist.
// Dlatego na czas skanowania wyciszamy FM, a przy graniu z SD/WWW podtrzymujemy bufor audio.
bool sdDuckedRadio = false;

void sdBusyBegin() {
  if (currentSource == SOURCE_RADIO && radioReady && !radioMuted) {
    si4703SetMute(true);
    sdDuckedRadio = true;
  }
}

void sdBusyPump() {
  if (currentSource == SOURCE_SD || currentSource == SOURCE_WEB) audio.loop();
}

void sdBusyEnd() {
  if (sdDuckedRadio) {
    sdDuckedRadio = false;
    if (!radioMuted && currentSource == SOURCE_RADIO) si4703SetMute(false);
  }
}

static void scanSdDirectoryImpl(const String &path) {
  sdMenuEntryCount = 0;

  File dir = SD.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return;
  }

  // Jeden przebieg po katalogu (wczesniej dwa): katalogi trafiaja od razu na liste,
  // pliki MP3 zbieramy osobno i dopisujemy za katalogami.
  static SdMenuEntry files[20];
  int fileCount = 0;

  while (File entry = dir.openNextFile()) {
    sdBusyPump();
    String entryName = sdEntryLabel(String(entry.name()));
    String entryPath = buildFullSdPath(path, entryName);
    if (entry.isDirectory()) {
      if (sdMenuEntryCount < 20 && !isHiddenSdDirectory(entry.name())) {
        sdMenuEntries[sdMenuEntryCount++] = { sdEntryLabel(entryPath), entryPath, true };
      }
    } else if (fileCount < 20) {
      String lower = entryPath;
      lower.toLowerCase();
      if (lower.endsWith(".mp3")) {
        files[fileCount++] = { sdEntryLabel(entryPath), entryPath, false };
      }
    }
    entry.close();
    if (sdMenuEntryCount >= 20 && fileCount >= 20) break;   // nic wiecej sie nie zmiesci
  }
  dir.close();

  for (int i = 0; i < fileCount && sdMenuEntryCount < 20; i++) {
    sdMenuEntries[sdMenuEntryCount++] = files[i];
  }
}

void scanSdDirectory(const String &path) {
  sdBusyBegin();
  scanSdDirectoryImpl(path);
  sdBusyEnd();
}

// --- Kasowanie utworu i formatowanie karty SD ---
void startDeleteConfirm() {
  if (!sdFileSelected()) return;
  SdMenuEntry &e = sdMenuEntries[menuIndex];
  confirmAction = CONFIRM_DELETE_FILE;
  confirmPath = e.path;
  confirmName = displayTrackName(e.name);
  confirmNote = "";
  confirmYes = false;
  confirmReturnUi = UI_MENU_SD;
  currentUi = UI_CONFIRM;
  oledUpdate(true);
}

void startCleanConfirm() {
  confirmAction = CONFIRM_CLEAN;
  confirmPath = "";
  confirmName = "Wszystkie pliki";
  confirmNote = "zostaną usunięte";
  confirmYes = false;
  confirmReturnUi = UI_SD_CLEAN;
  currentUi = UI_CONFIRM;
  oledUpdate(true);
}

// Ekran z dlugim paskiem postepu (czyszczenie karty).
void drawProgressScreen(const char *title, const String &line, int pct, const char *hint) {
  pct = constrain(pct, 0, 100);
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_te);
  oled.drawUTF8((128 - oled.getUTF8Width(title)) / 2, 11, title);
  oled.drawHLine(0, 13, 128);
  oled.setFont(u8g2_font_NokiaSmallPlain_te);
  String s = line;
  while (s.length() > 3 && oled.getUTF8Width(s.c_str()) > 124) removeLastUtf8Char(s);
  oled.drawUTF8((128 - oled.getUTF8Width(s.c_str())) / 2, 27, s.c_str());
  oled.drawFrame(4, 32, 120, 12);
  oled.drawBox(6, 34, (116 * pct) / 100, 8);
  oled.setFont(u8g2_font_6x12_te);
  char b[8];
  snprintf(b, sizeof(b), "%d%%", pct);
  oled.drawUTF8((128 - oled.getUTF8Width(b)) / 2, 56, b);
  if (hint) {
    oled.setFont(u8g2_font_NokiaSmallPlain_te);
    oled.drawUTF8((128 - oled.getUTF8Width(hint)) / 2, 63, hint);
  }
  oledSend();
}

// Przerwanie czyszczenia: przycisk Wstecz (ENC1 lub BTN3).
static bool cleanAborted = false;
static bool cleanAbortRequested() {
  if (digitalRead(ENC1_KEY) == LOW || digitalRead(BTN3_PIN) == LOW) cleanAborted = true;
  return cleanAborted;
}

// Liczy wszystkie pliki i katalogi (do paska postepu). Podkatalogi sa odwiedzane po zamknieciu
// biezacego katalogu, zeby nie przekroczyc limitu jednocześnie otwartych plikow.
static int sdCountEntries(const String &path) {
  File dir = SD.open(path.c_str());
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return 0; }
  int n = 0;
  std::vector<String> subs;
  while (File e = dir.openNextFile()) {
    n++;
    if (e.isDirectory()) subs.push_back(buildFullSdPath(path, sdEntryLabel(String(e.name()))));
    e.close();
  }
  dir.close();
  for (size_t i = 0; i < subs.size(); i++) n += sdCountEntries(subs[i]);
  return n;
}

// Usuwa zawartosc katalogu (rekurencyjnie). Po kazdym skasowaniu otwieramy katalog od nowa,
// a elementy, ktorych nie da sie usunac, omijamy (skip) - dzieki temu nie ma petli bez konca.
static void sdWipeDir(const String &path, int &removed, int &failed, int total, const char *title) {
  int skip = 0;
  for (;;) {
    if (cleanAbortRequested()) return;
    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    File entry = dir.openNextFile();
    for (int i = 0; i < skip && entry; i++) { entry.close(); entry = dir.openNextFile(); }
    if (!entry) { dir.close(); return; }
    String name = sdEntryLabel(String(entry.name()));
    bool isDir = entry.isDirectory();
    entry.close();
    dir.close();

    String full = buildFullSdPath(path, name);
    bool ok;
    if (isDir) {
      sdWipeDir(full, removed, failed, total, title);
      if (cleanAborted) return;
      ok = SD.rmdir(full.c_str());
    } else {
      ok = SD.remove(full.c_str());
    }
    if (ok) removed++; else { failed++; skip++; }
    int done = removed + failed;
    if ((done & 3) == 0 || done == total)
      drawProgressScreen(title, "Usuwanie plików", total > 0 ? done * 100 / total : 100, "Wstecz = przerwij");
  }
}

void sdCleanCard() {
  recorderStop(true);
  if (currentSource == SOURCE_SD || currentSource == SOURCE_WEB) stopPlayback();
  if (!sdReady) { showUiMessage("Brak karty SD", "", 2000); return; }
  const char *title = "Czyszczenie karty";
  cleanAborted = false;
  int removed = 0, failed = 0;
  sdBusyBegin();
  drawProgressScreen(title, "Liczenie plików", 0, nullptr);
  int total = sdCountEntries("/");
  sdWipeDir("/", removed, failed, total, title);
  sdBusyEnd();
  currentSdPath = "/";
  sdMenuEntryCount = 0;
  sdFileCount = 0;
  currentTrackPath = "";
  currentTrackIndex = -1;
  sdInfoUpdate();
  if (cleanAborted) showUiMessage("Przerwano", "karta częściowo wyczyszczona", 3500);
  else if (failed > 0) showUiMessage("Nie usunięto", String(failed) + " elementów", 3500);
  else showUiMessage("Karta wyczyszczona", String(removed) + " usuniętych", 3000);
}

void sdDeleteConfirmedTrack() {
  String path = confirmPath;
  if (currentSource == SOURCE_SD && currentTrackPath == path) stopPlayback();
  bool ok = SD.remove(path.c_str());
  scanSdDirectory(currentSdPath);
  if (menuIndex >= sdMenuEntryCount) menuIndex = max(0, sdMenuEntryCount - 1);
  if (menuIndex < menuScroll) menuScroll = menuIndex;
  if (menuIndex >= menuScroll + listVisibleRows()) menuScroll = menuIndex - listVisibleRows() + 1;
  sdInfoUpdate();
  showUiMessage(ok ? "Utwór usunięty" : "Błąd usuwania", ok ? "" : "sprawdź kartę SD", 2000);
}

void confirmEnter() {
  bool yes = confirmYes;
  ConfirmAction action = confirmAction;
  currentUi = confirmReturnUi;
  confirmAction = CONFIRM_NONE;
  if (!yes) return;
  if (action == CONFIRM_DELETE_FILE) sdDeleteConfirmedTrack();
  else if (action == CONFIRM_CLEAN) sdCleanCard();
}

// ================== STALE ZAPISANE NA STALE ==================
// Stacje sprawdzone przez uzytkownika. Sa ladowane przy kazdym uruchomieniu.
const float presetFmStations[] = {

};
const char* presetFmStationNames[] = {

};
const int presetFmStationCount = sizeof(presetFmStations) / sizeof(presetFmStations[0]);

const char* fmStationNameForFrequency(float freqMHz) {
  for (int i = 0; i < presetFmStationCount; i++) {
    if (fabsf(presetFmStations[i] - freqMHz) < 0.05f) {
      return presetFmStationNames[i];
    }
  }
  return "";
}

void loadPresetStations() {
  fmStationCount = 0;
  currentStationIndex = -1;
  for (int i = 0; i < presetFmStationCount && fmStationCount < FM_MAX_STATIONS; i++) {
    fmStations[fmStationCount++] = presetFmStations[i];
    fmStationNames[fmStationCount - 1] = presetFmStationNames[i];
  }
  sortFmStations();
}

void addStationIfNew(float freqMHz) {
  for (int i = 0; i < fmStationCount; i++) {
    if (fabsf(fmStations[i] - freqMHz) < 0.05f) return;
  }
  if (fmStationCount < FM_MAX_STATIONS) {
    fmStations[fmStationCount++] = freqMHz;
    fmStationNames[fmStationCount - 1] = fmStationNameForFrequency(freqMHz);
    sortFmStations();
  }
}

// ================== POMIAR AKUMULATORA ==================
// Pomiar pojedynczego ogniwa Li-Ion 18650.
// GPIO3 (ADC1) otrzymuje polowe napiecia ogniwa przez dzielnik 1:1.
#define BATTERY_UPDATE_MS 1000

float batteryVoltageCached = 0.0f;
uint32_t lastBatteryRead = 0;

void batteryInit() {
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
}

void updateBatteryMeasurement(bool force = false) {
  if (!force && !batteryRefreshRequested && millis() - lastBatteryRead < BATTERY_UPDATE_MS) return;
  batteryRefreshRequested = false;
  lastBatteryRead = millis();

  uint32_t sumMv = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    sumMv += analogReadMilliVolts(BATTERY_ADC_PIN);
    delayMicroseconds(200);
  }

  float pinVoltage = (sumMv / (float)BATTERY_ADC_SAMPLES) / 1000.0f;
  float batteryVoltage = pinVoltage * BATTERY_DIVIDER_RATIO;

  if (batteryVoltage > BATTERY_MAX_VOLTAGE) batteryVoltage = BATTERY_MAX_VOLTAGE;
  if (batteryVoltage < 0.0f) batteryVoltage = 0.0f;

  batteryVoltageCached = batteryVoltage;
}

bool isLowBattery() {
  updateBatteryMeasurement();
  return batteryVoltageCached < LOW_BATTERY_WARNING_VOLTAGE;
}

float readBatteryVoltage() {
  updateBatteryMeasurement();
  return batteryVoltageCached;
}

// Ikona baterii w prawym gornym rogu (poziom wg napiecia; przy niskim stanie miga).
void drawBatteryVoltage() {
  bool lowBattery = isLowBattery();
  if (lowBattery && (millis() / 500) % 2 != 0) return;

  float level = (batteryVoltageCached - LOW_BATTERY_WARNING_VOLTAGE) /
                (BATTERY_FULL_VOLTAGE - LOW_BATTERY_WARNING_VOLTAGE);
  level = constrain(level, 0.0f, 1.0f);

  oled.setDrawColor(1);
  oled.drawFrame(103, 1, 21, 9);
  oled.drawBox(124, 3, 2, 5);                 // wypustka (biegun)
  int fill = (int)(level * 17.0f + 0.5f);
  if (fill > 0) oled.drawBox(105, 3, fill, 5);
}

int findClosestStationIndex(uint32_t freqKHz) {
  if (fmStationCount == 0) return -1;
  int best = 0;
  uint32_t bestDiff = 0xFFFFFFFF;
  for (int i = 0; i < fmStationCount; i++) {
    uint32_t stFreq = (uint32_t)(fmStations[i] * 1000.0f + 0.5f);
    uint32_t diff = (stFreq > freqKHz) ? (stFreq - freqKHz) : (freqKHz - stFreq);
    if (diff < bestDiff) { bestDiff = diff; best = i; }
  }
  return best;
}

void scanFmStations() {
  if (!radioReady) {
    Serial.println(F("BLAD: Tuner Si4703 niedostepny, nie mozna skanowac."));
    return;
  }

  // Zachowujemy na stale stacje sprawdzone przez uzytkownika,
  // a znalezione podczas skanowania dopisujemy do listy.
  loadPresetStations();

  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB10_te);
  oled.drawUTF8(4, 30, "Skanowanie FM");
  oled.setFont(u8g2_font_6x12_te);
  oled.drawUTF8(4, 48, "To może potrwać...");
  oledSend();

  Serial.println(F("--- Skanowanie pasma FM ---"));

  si4703SetFrequencyKHz(RADIO_MIN_KHZ);
  radioFrequencyKHz = RADIO_MIN_KHZ;
  delay(100);

  uint32_t lastFreq = RADIO_MIN_KHZ;
  bool wrapped = false;

  while (fmStationCount < FM_MAX_STATIONS && !wrapped) {
    bool found = si4703Seek(true); // szuka w gore od biezacej czestotliwosci
    uint32_t freqKHz = radioFrequencyKHz; // zaktualizowane przez si4703Seek()

    if (freqKHz <= lastFreq) {
      // Tuner "zawinal" pasmo z powrotem na dol - pelny obrot zakonczony.
      wrapped = true;
    }
    lastFreq = freqKHz;

    if (found) {
      float freqMHz = freqKHz / 1000.0f;
      int existingIndex = findClosestStationIndex(freqKHz);
      bool isExisting = existingIndex >= 0 &&
                        fabsf(fmStations[existingIndex] - freqMHz) < 0.05f;

      if (isExisting) {
        currentStationIndex = existingIndex;
      } else if (fmStationCount < FM_MAX_STATIONS) {
        fmStations[fmStationCount++] = freqMHz;
        fmStationNames[fmStationCount - 1] = fmStationNameForFrequency(freqMHz);
        sortFmStations();
        currentStationIndex = findClosestStationIndex(freqKHz);
      }

      Serial.printf("  Znaleziono: %.1f MHz\n", freqKHz / 1000.0f);
      const char* stationName = fmStationNameForFrequency(freqMHz);
      if (stationName[0] != '\0') {
        Serial.printf("    Stacja: %s\n", stationName);
      }
    }
  }

  sortFmStations();

  Serial.printf("Skanowanie zakonczone. Zapisano %d stacji.\n", fmStationCount);

  currentUi = UI_MENU_RADIO;
  menuIndex = 0;
  menuScroll = 0;
  oledUpdate(true);
}

void beginDateTimeEdit() {
  editHour = rtcHour;
  editMinute = rtcMinute;
  editDay = rtcDay;
  editMonth = rtcMonth;
  editYear = rtcYear;
  datetimeEditField = 0;
  dateTimeSaved = false;
  currentUi = UI_MENU_DATETIME;
  oledUpdate(true);
}

void adjustDateTimeField(int direction) {
  switch (datetimeEditField) {
    case 0: editHour = (editHour + direction + 24) % 24; break;
    case 1: editMinute = (editMinute + direction + 60) % 60; break;
    case 2: editDay = constrain((int)editDay + direction, 1, 31); break;
    case 3: editMonth = constrain((int)editMonth + direction, 1, 12); break;
    case 4: editYear = constrain((int)editYear + direction, 2000, 2099); break;
  }
  dateTimeSaved = false;
  oledUpdate(true);
}

void saveDateTimeEdit() {
  if (ds3231SetTime(editYear, editMonth, editDay, editHour, editMinute, 0)) {
    rtcReady = true;
    Serial.println(F("Czas DS3231 ustawiony."));
    dateTimeSaved = true;
  } else {
    Serial.println(F("BLAD: Nie mozna zapisac daty/czasu DS3231."));
  }
  oledUpdate(true);
}

// Wiersze-naglowki sekcji w Informacjach (kursor ich nie zatrzymuje).
bool infoIsHeader(int index) {
  return index == 0 || index == 5 || index == 9 || index == 13 || index == 20 || index == 25;
}

// Kolejnosc wyswietlania: System, Karta SD, Siec, Radio FM, Odtwarzanie, Czas.
// Tabela mapuje pozycje na ekranie -> numer wiersza w infoRow().
const uint8_t infoOrder[28] = {
  5, 6, 7, 8, 9,                // System
  10, 11, 12, 13,               // Karta SD
  14, 15, 16, 17,               // Siec
  18, 19, 20, 21, 22, 23, 24,   // Radio FM
  0, 1, 2, 3, 4,               // Odtwarzanie
  25, 26, 27                    // Czas
};

void infoEnsureVisible() {
  int top = menuIndex;
  if (top > 0 && infoIsHeader(top - 1)) top--;   // pokaz naglowek sekcji nad pozycja
  if (top < menuScroll) menuScroll = top;
  if (menuIndex >= menuScroll + infoVisibleRows) menuScroll = menuIndex - infoVisibleRows + 1;
  menuScroll = constrain(menuScroll, 0, infoItemCount - infoVisibleRows);
}

void menuUp() {
  if (currentUi == UI_MENU_MAIN) {
    menuIndex = (menuIndex + mainMenuCount - 1) % mainMenuCount;
    menuScroll = 0;
    oledUpdate(true);
    return;
  }
  if (currentUi == UI_MENU_INFO) {
    do { menuIndex = (menuIndex + infoItemCount - 1) % infoItemCount; } while (infoIsHeader(menuIndex));
    infoEnsureVisible();
    oledUpdate(true);
    return;
  }
  if (currentUi == UI_TEXT_ENTRY) { entryChangeChar(-1); return; }
  if (currentUi == UI_CONFIRM) { confirmYes = !confirmYes; oledUpdate(true); return; }
  if (currentUi == UI_SD_CLEAN) return;   // jedna pozycja - nie ma po czym przewijac
  if (currentUi == UI_PLAYER || currentUi == UI_MENU_DATETIME) return;
  if (menuIndex > 0) menuIndex--;
  else if (currentUi == UI_MENU_SETTINGS || currentUi == UI_MENU_WEB || currentUi == UI_MENU_RADIO) {
    // Z pierwszej pozycji przeskok na koniec listy
    menuIndex = listItemCount() - 1;
    menuScroll = max(0, listItemCount() - listVisibleRows());
  }
  if (menuIndex < menuScroll) menuScroll = menuIndex;
  oledUpdate(true);
}

void menuDown() {
  if (currentUi == UI_MENU_MAIN) {
    menuIndex = (menuIndex + 1) % mainMenuCount;
    menuScroll = 0;
    oledUpdate(true);
    return;
  }
  if (currentUi == UI_MENU_INFO) {
    do { menuIndex = (menuIndex + 1) % infoItemCount; } while (infoIsHeader(menuIndex));
    infoEnsureVisible();
    oledUpdate(true);
    return;
  }
  if (currentUi == UI_TEXT_ENTRY) { entryChangeChar(1); return; }
  if (currentUi == UI_CONFIRM) { confirmYes = !confirmYes; oledUpdate(true); return; }
  if (currentUi == UI_SD_CLEAN) return;   // jedna pozycja - nie ma po czym przewijac
  if (currentUi == UI_PLAYER || currentUi == UI_MENU_DATETIME) return;
  int maxItems = listItemCount();

  if (menuIndex < maxItems - 1) menuIndex++;
  else if (currentUi == UI_MENU_SETTINGS || currentUi == UI_MENU_WEB || currentUi == UI_MENU_RADIO) {
    // Z ostatniej pozycji przeskok na poczatek listy
    menuIndex = 0;
    menuScroll = 0;
  }
  if (menuIndex >= menuScroll + listVisibleRows()) menuScroll++;
  oledUpdate(true);
}

void menuEnter() {
  if (currentUi == UI_PLAYER) {
    currentUi = UI_MENU_MAIN; menuIndex = tileForCurrentSource(); menuScroll = 0;
  }
  else if (currentUi == UI_MENU_MAIN) {
    int mid = mainMenuOrder[menuIndex];
    lastMainTile = menuIndex;   // zapamietaj kafelek, do ktorego wchodzimy
    if (mid == 1) {
      currentSdPath = "/";
      scanSdDirectory(currentSdPath);
      currentUi = UI_MENU_SD; menuIndex = 0; menuScroll = 0;
    }
    else if (mid == 2) {
      currentUi = UI_MENU_RADIO; menuIndex = 0; menuScroll = 0;
    }
    else if (mid == 3) {
      scanFmStations(); // sama zmienia currentUi po zakonczeniu
    }
    else if (mid == 4) {
      dateTimeReturnUi = UI_MENU_MAIN;
      beginDateTimeEdit();
    }
    else if (mid == 6) {
      currentUi = UI_MENU_WEB;
      menuIndex = webStationIndex;   // zawsze ostatnia wybrana stacja WWW
      menuScroll = (menuIndex >= itemsPerPage) ? menuIndex - itemsPerPage + 1 : 0;
    }
    else if (mid == 7) {
      currentUi = UI_MENU_SETTINGS;
      menuIndex = 0;
      menuScroll = 0;
    }
  }
  else if (currentUi == UI_MENU_SD) {
    if (sdMenuEntryCount > 0) {
      SdMenuEntry &entry = sdMenuEntries[menuIndex];
      if (entry.isDirectory) {
        currentSdPath = entry.path;
        scanSdDirectory(currentSdPath);
        menuIndex = 0;
        menuScroll = 0;
      } else if (startPlayback(entry.path.c_str())) {
        currentTrackIndex = -1;
        currentUi = UI_PLAYER;
      }
    }
  }
  else if (currentUi == UI_MENU_RADIO) {
    if (fmStationCount > 0) {
      stopPlayback();
      currentStationIndex = menuIndex;
      setRadioFrequencyKHz((uint32_t)(fmStations[menuIndex] * 1000.0f + 0.5f));
      currentSource = SOURCE_RADIO;
      radioMuted = false;
      if (radioReady) si4703SetMute(false);
      digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
      currentUi = UI_PLAYER;
    }
  }
  else if (currentUi == UI_MENU_WEB) {
    webFailStreak = 0;
    startWebStation(menuIndex);   // przy sukcesie samo przechodzi na ekran odtwarzacza
  }
  else if (currentUi == UI_MENU_SETTINGS) {
    if (menuIndex == 0) { currentUi = UI_MENU_WIFI; menuIndex = 0; menuScroll = 0; }
    else if (menuIndex == 1) { currentUi = UI_MENU_SOUND; menuIndex = 0; menuScroll = 0; }
    else if (menuIndex == 2) { currentUi = UI_MENU_DISPLAY; menuIndex = lightMode ? 1 : 0; menuScroll = 0; }
    else if (menuIndex == 3) { dateTimeReturnUi = UI_MENU_SETTINGS; beginDateTimeEdit(); }
    else if (menuIndex == 4) {
      if (!sdReady) showUiMessage("Brak karty SD", "", 2000);
      else { sdInfoUpdate(); currentUi = UI_SD_CLEAN; menuIndex = 0; menuScroll = 0; }
    }
    else if (menuIndex == 5) { sdInfoUpdate(); currentUi = UI_MENU_INFO; menuIndex = 1; menuScroll = 0; }
  }
  else if (currentUi == UI_MENU_WIFI) {
    wifiMenuEnter(menuIndex);
  }
  else if (currentUi == UI_MENU_DISPLAY) {
    lightMode = (menuIndex == 1);
    displaySaveSettings();
  }
  else if (currentUi == UI_MENU_SOUND) {
    soundEnter(menuIndex);   // pasma i balans reguluje sie obrotem ENC1
  }
  else if (currentUi == UI_WIFI_SCAN) {
    wifiScanEnter(menuIndex);
  }
  else if (currentUi == UI_SD_CLEAN) {
    if (!sdReady) showUiMessage("Brak karty SD", "", 2000);
    else startCleanConfirm();
  }
  else if (currentUi == UI_CONFIRM) {
    confirmEnter();
  }
  else if (currentUi == UI_TEXT_ENTRY) {
    entryInsert();
  }
  oledUpdate(true);
}

void menuBack() {
  if (currentUi == UI_MENU_SD && currentSdPath != "/") {
    int slash = currentSdPath.lastIndexOf('/');
    currentSdPath = (slash <= 0) ? "/" : currentSdPath.substring(0, slash);
    scanSdDirectory(currentSdPath);
    menuIndex = 0; menuScroll = 0;
  } else if (currentUi == UI_MENU_INFO) {
    currentUi = UI_MENU_SETTINGS; menuIndex = 5; menuScroll = max(0, 5 - itemsPerPage + 1);
  } else if (currentUi == UI_MENU_DATETIME && dateTimeReturnUi == UI_MENU_SETTINGS) {
    currentUi = UI_MENU_SETTINGS; menuIndex = 3; menuScroll = max(0, 3 - itemsPerPage + 1);
  } else if (currentUi == UI_MENU_DISPLAY) {
    currentUi = UI_MENU_SETTINGS; menuIndex = 2; menuScroll = 0;
  } else if (currentUi == UI_MENU_SOUND) {
    currentUi = UI_MENU_SETTINGS; menuIndex = 1; menuScroll = 0;
  } else if (currentUi == UI_MENU_SD || currentUi == UI_MENU_RADIO ||
             currentUi == UI_MENU_DATETIME ||
             currentUi == UI_MENU_WEB || currentUi == UI_MENU_SETTINGS) {
    currentUi = UI_MENU_MAIN; menuIndex = lastMainTile; menuScroll = 0;
  } else if (currentUi == UI_SD_CLEAN) {
    currentUi = UI_MENU_SETTINGS; menuIndex = 4; menuScroll = max(0, 4 - itemsPerPage + 1);
  } else if (currentUi == UI_CONFIRM) {
    currentUi = confirmReturnUi;
  } else if (currentUi == UI_MENU_MAIN) {
    currentUi = UI_PLAYER;
  } else if (currentUi == UI_MENU_WIFI) {
    currentUi = UI_MENU_SETTINGS; menuIndex = 0; menuScroll = 0;
  } else if (currentUi == UI_WIFI_SCAN) {
    currentUi = UI_MENU_WIFI; menuIndex = 0; menuScroll = 0;
  } else if (currentUi == UI_TEXT_ENTRY) {
    entryCancel();
  }
  oledUpdate(true);
}


// ================== KOMENDY Z TERMINALA ==================

void printHelp() {
  Serial.println(F("Dostepne komendy:"));
  Serial.println(F("  help              - ta pomoc"));
  Serial.println(F("  play [plik]       - odtworz .mp3 z SD"));
  Serial.println(F("  stop              - zatrzymaj / standby"));
  Serial.println(F("  vol <0-15>        - ustaw glosnosc"));
  Serial.println(F("  radio on / off    - wlacz radio / wycisz radio"));
  Serial.println(F("  tune <MHz>        - strojenie recznie, np: tune 101.5"));
  Serial.println(F("  seekup / seekdown - automatyczne szukanie stacji"));
  Serial.println(F("  scan              - pelne skanowanie pasma FM i zapis stacji"));
  Serial.println(F("  time              - odczytaj czas z DS3231"));
  Serial.println(F("  info              - otworz menu informacji (tez: Ustaw > Info)"));
  Serial.println(F("  settime DD.MM.YYYY HH:MM:SS - ustaw czas DS3231"));
  Serial.println(F("--- TESTOWANIE MENU ---"));
  Serial.println(F("  up, down, enter, back - nawigacja po interfejsie graficznym"));
  Serial.println(F("--- ENKODERY ---"));
  Serial.println(F("  ENC1 obrot     - glosnosc"));
  Serial.println(F("  ENC1 przycisk  - w menu: COFNIJ | w radiu: MUTE | w SD: PAUZA"));
  Serial.println(F("  ENC2 obrot     - w menu: przewijanie | radio: stacja | SD: utwor"));
  Serial.println(F("  ENC2 przycisk  - w odtwarzaczu: MENU | w menu: WYBIERZ"));
  Serial.println(F("  Podczas odtwarzania MP3 przyciski ENC1 i ENC2 zamieniaja sie funkcjami"));
  Serial.println(F("--- PRZYCISKI (modul 4-key) ---"));
  Serial.println(F("  BTN1 (GPIO39) - PREV: menu gora | radio: -100 kHz | SD: predkosc +0.5x"));
  Serial.println(F("  BTN2 (GPIO40) - NEXT: menu dol  | radio: +100 kHz | SD: predkosc -0.5x"));
  Serial.println(F("  BTN3 (GPIO41) - odtwarzacz: zrodlo Radio/SD | menu: COFNIJ"));
  Serial.println(F("  BTN4 (GPIO42) - odtwarzacz: wejdz do MENU   | menu: WYBIERZ (OK)"));
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  int sp = cmd.indexOf(' ');
  String verb = (sp == -1) ? cmd : cmd.substring(0, sp);
  String arg  = (sp == -1) ? ""  : cmd.substring(sp + 1);
  verb.toLowerCase(); arg.trim();

  if (verb == "info") { sdInfoUpdate(); currentUi = UI_MENU_INFO; menuIndex = 1; menuScroll = 0; oledUpdate(true); }
  else if (verb == "up") menuUp();
  else if (verb == "down") menuDown();
  else if (verb == "enter") menuEnter();
  else if (verb == "back") menuBack();

  else if (verb == "help") printHelp();
  else if (verb == "seek" && arg.length() > 0) {
    int seconds = arg.toInt();
    if (currentSource == SOURCE_SD) {
      seekAudioSeconds(seconds);
    }
  }
  else if (verb == "play") {
    if (arg.length() == 0) {
      String first = findFirstAudioFile();
      if (first.length() > 0) startPlayback(first.c_str());
      else Serial.println(F("Brak plikow .mp3 na karcie SD."));
    } else { startPlayback(arg.c_str()); }
  }
  else if (verb == "stop") { stopPlayback(); currentUi = UI_PLAYER; oledUpdate(true); }
  else if (verb == "vol") {
    if (dacIsActiveSource()) dacVolRef() = constrain(arg.toInt(), 0, DAC_VOLUME_UI_MAX);
    else volumeLevel = constrain(arg.toInt(), 0, 15);
    applyVolume();
    oledUpdate(true);
  }
  else if (verb == "radio" && arg == "on") {
    stopPlayback();
    currentSource = SOURCE_RADIO;
    radioMuted = false;
    if (radioReady) si4703SetMute(false);
    digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
    currentUi = UI_PLAYER;
    oledUpdate(true);
  }
  else if (verb == "radio" && arg == "off") {
    radioMuted = true;
    if (radioReady) si4703SetMute(true);
    oledUpdate(true);
  }
  else if (verb == "tune") {
    stopPlayback();
    uint32_t freqKHz = (uint32_t)(arg.toFloat() * 1000.0f + 0.5f);
    setRadioFrequencyKHz(freqKHz);
    currentSource = SOURCE_RADIO;
    radioMuted = false;
    if (radioReady) si4703SetMute(false);
    digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
    currentStationIndex = -1;
    oledUpdate(true);
  }
  else if (verb == "seekup" || verb == "seekdown") {
    stopPlayback();
    currentSource = SOURCE_RADIO;
    digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
    radioMuted = false;
    if (radioReady) si4703SetMute(false);
    si4703Seek(verb == "seekup");
    currentStationIndex = -1;
    oledUpdate(true);
  }
  else if (verb == "scan") {
    scanFmStations();
  }
  else if (verb == "time") {
    printRtcTime();
    oledUpdate(true);
  }
  else if (verb == "setdate" || verb == "settime") {
    int day, month, year, hour, minute, second;
    if (sscanf(arg.c_str(), "%d.%d.%d %d:%d:%d",
               &day, &month, &year, &hour, &minute, &second) == 6) {
      if (ds3231SetTime((uint16_t)year, (uint8_t)month, (uint8_t)day,
                        (uint8_t)hour, (uint8_t)minute, (uint8_t)second)) {
        rtcReady = true;
        Serial.println(F("Czas DS3231 ustawiony."));
        printRtcTime();
        oledUpdate(true);
      } else {
        Serial.println(F("BLAD: Nieprawidlowy czas lub brak DS3231."));
      }
    } else {
      Serial.println(F("Uzycie: settime DD.MM.YYYY HH:MM:SS"));
    }
  }
  else { Serial.println("Nieznana komenda."); }
}


// ================== INTERFEJS OLED ==================

void oledInit() {
  oled.begin();

  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB10_te);

  const char* title = "RADIO PRO";
  int w = oled.getUTF8Width(title);
  oled.drawUTF8((128 - w) / 2, 35, title);

  oled.setFont(u8g2_font_6x12_te);
  oled.drawUTF8(30, 55, "Wczytywanie...");
  oledSend();

  delay(1000);
  oledReady = true;
}

// --- Usuwa ostatni znak UTF-8 (polskie litery to 2 bajty) - bezpieczne obcinanie napisow ---
void removeLastUtf8Char(String &text) {
  int len = text.length();
  if (len == 0) return;
  int cut = len - 1;
  while (cut > 0 && (((uint8_t)text[cut]) & 0xC0) == 0x80) cut--;  // pomin bajty kontynuacji
  text.remove(cut);
}

// --- Nazwa utworu bez rozszerzenia ".mp3" (tylko do wyswietlania) ---
String displayTrackName(const String &name) {
  if (name.length() >= 4) {
    String ext = name.substring(name.length() - 4);
    ext.toLowerCase();
    if (ext == ".mp3") return name.substring(0, name.length() - 4);
  }
  return name;
}

// --- Plynne przewijanie dlugich tekstow (marquee) ---
// Przesuniecie liczone z czasu (px/s), wiec nie zalezy od tego jak czesto
// odswiezamy ekran. Na poczatku kazdego cyklu jest krotka pauza, zeby
// dalo sie przeczytac poczatek nazwy.
#define MARQUEE_FRAME_MS        40     // ~25 klatek/s podczas przewijania
#define MARQUEE_SPEED_PX_PER_S  30
#define MARQUEE_START_PAUSE_MS  1000
#define MARQUEE_GAP_PX          24

String marqueeKeys[2] = { "", "" };
uint32_t marqueeStarts[2] = { 0, 0 };
bool marqueeActive = false;   // ustawiane podczas rysowania klatki

// slot 0 / 1 - dwa niezalezne napisy moga przewijac sie jednoczesnie
int marqueeOffset(const String &text, int cycleWidth, int slot = 0) {
  if (text != marqueeKeys[slot]) {   // nowy tekst -> przewijanie od poczatku
    marqueeKeys[slot] = text;
    marqueeStarts[slot] = millis();
  }
  marqueeActive = true;

  uint32_t cycleMs = MARQUEE_START_PAUSE_MS +
                     (uint32_t)cycleWidth * 1000UL / MARQUEE_SPEED_PX_PER_S;
  uint32_t t = (millis() - marqueeStarts[slot]) % cycleMs;
  if (t < MARQUEE_START_PAUSE_MS) return 0;
  return (int)((t - MARQUEE_START_PAUSE_MS) * MARQUEE_SPEED_PX_PER_S / 1000UL);
}

// Tekst w nawiasach [ ] przy prawej krawedzi (margines 2 px). Czcionka Nokia nie ma znakow [ ],
// wiec nawiasy rysujemy kreskami. 'baseline' = linia podstawowa tekstu.
int menuHintWidth() {   // szerokosc zajeta od prawej krawedzi ekranu przez "[Menu]"
  oled.setFont(u8g2_font_NokiaSmallPlain_te);
  return oled.getUTF8Width("Menu") + 8;
}

void drawBracketText(const char* text, int baseline) {
  oled.setFont(u8g2_font_NokiaSmallPlain_te);
  int w = oled.getUTF8Width(text);   // szerokosc z 1 px pustej kolumny za ostatnia litera
  int inkEnd = 122;                  // ostatnia zapalona kolumna tekstu; prawy nawias w kol. 124-125
  int inkStart = inkEnd - (w - 1) + 1;
  int x0 = inkStart - 4;             // lewy nawias: kol. x0..x0+1, luka 1 px, tekst - identycznie jak po prawej
  int top = baseline - 6;            // nawiasy maja wysokosc wielkich liter (7 px)
  oled.drawUTF8(inkStart - 1, baseline, text);   // tekst o 1 px w lewo; nawiasy bez zmian
  oled.drawVLine(x0, top, 7);
  oled.drawHLine(x0, top, 2);
  oled.drawHLine(x0, baseline, 2);
  oled.drawVLine(125, top, 7);
  oled.drawHLine(124, top, 2);
  oled.drawHLine(124, baseline, 2);
}

void drawMenuHint() { drawBracketText("Menu", 62); }

void drawStatusBar() {
  oled.setFont(u8g2_font_6x12_te);
  char timeStr[6];
  if (rtcReady) snprintf(timeStr, sizeof(timeStr), "%02d:%02d", rtcHour, rtcMinute);
  else snprintf(timeStr, sizeof(timeStr), "--:--");
  oled.drawUTF8(2, 9, timeStr);

  const char* src = (currentSource == SOURCE_RADIO) ? "FM" : (currentSource == SOURCE_SD) ? "SD" : (currentSource == SOURCE_WEB) ? "Internet" : "--";
  int srcWidth = oled.getUTF8Width(src);
  if (currentSource == SOURCE_SD && fabsf(playbackSpeed - DEFAULT_PLAYBACK_SPEED) > 0.01f) {
    char speedText[8];
    snprintf(speedText, sizeof(speedText), "%.1fx", playbackSpeed);
    int speedWidth = oled.getUTF8Width(speedText);
    int groupWidth = srcWidth + 4 + speedWidth;
    int groupX = (128 - groupWidth) / 2;
    oled.drawUTF8(groupX, 9, src);
    oled.drawUTF8(groupX + srcWidth + 4, 9, speedText);
  } else {
    oled.drawUTF8((128 - srcWidth) / 2, 9, src);
  }
  drawBatteryVoltage();

  oled.drawHLine(0, 12, 128);
}

// Naglowek wysrodkowany; opcjonalny dopisek (np. nazwa sieci) mniejsza czcionka i przycinany do szerokosci.
void drawHeader(const char* title, const char* suffix = nullptr, bool line = true) {
  oled.setFont(u8g2_font_6x12_te);
  int titleW = oled.getUTF8Width(title);

  String suf = suffix ? String(suffix) : String("");
  int sufW = 0;
  if (suf.length() > 0) {
    oled.setFont(u8g2_font_NokiaSmallPlain_te);
    while (suf.length() > 1 && titleW + 4 + oled.getUTF8Width(suf.c_str()) > 124) removeLastUtf8Char(suf);
    sufW = oled.getUTF8Width(suf.c_str()) + 4;
  }

  int x = (128 - titleW - sufW) / 2;
  if (x < 0) x = 0;
  oled.setFont(u8g2_font_6x12_te);
  oled.drawUTF8(x, 9, title);
  if (suf.length() > 0) {
    oled.setFont(u8g2_font_NokiaSmallPlain_te);
    oled.drawUTF8(x + titleW + 4, 9, suf.c_str());
  }
  if (line) oled.drawHLine(0, 12, 128);
  oled.setFont(u8g2_font_6x12_te);
}

void drawSdClean() {
  drawHeader("Wyczyść kartę SD");
  oled.setFont(u8g2_font_NokiaSmallPlain_te);
  if (sdReady && sdInfoValid && sdTotalBytes > 0) {
    int pct = (int)(sdUsedBytes * 100 / sdTotalBytes);
    oled.drawFrame(3, 17, 122, 9);
    oled.drawBox(5, 19, (118 * pct) / 100, 5);
    String info = String("Zajęte ") + formatBytes(sdUsedBytes) + " / " + formatBytes(sdTotalBytes);
    oled.drawUTF8((128 - oled.getUTF8Width(info.c_str())) / 2, 35, info.c_str());
  } else {
    const char* none = "Brak danych o karcie";
    oled.drawUTF8((128 - oled.getUTF8Width(none)) / 2, 28, none);
  }
  oled.setFont(u8g2_font_6x12_te);
  int yPos = 52;   // jedyna pozycja, zawsze zaznaczona
  oled.setDrawColor(1);
  oled.drawBox(0, yPos - 9, 128, 12);
  oled.setDrawColor(0);
  const char* label = "Wyczyść kartę";
  oled.drawUTF8((128 - oled.getUTF8Width(label)) / 2, yPos, label);
  oled.setDrawColor(1);
}

void drawConfirm() {
  drawHeader(confirmAction == CONFIRM_CLEAN ? "Wyczyścić kartę?" : "Usunąć utwór?");
  oled.setFont(u8g2_font_NokiaSmallPlain_te);
  String n = confirmName;
  while (n.length() > 1 && oled.getUTF8Width(n.c_str()) > 124) removeLastUtf8Char(n);
  oled.drawUTF8((128 - oled.getUTF8Width(n.c_str())) / 2, 27, n.c_str());
  if (confirmNote.length() > 0)
    oled.drawUTF8((128 - oled.getUTF8Width(confirmNote.c_str())) / 2, 38, confirmNote.c_str());
  oled.setFont(u8g2_font_6x12_te);
  const char* labels[2] = { "Nie", "Tak" };
  for (int i = 0; i < 2; i++) {
    int x = (i == 0) ? 12 : 70;
    bool sel = (i == 0) ? !confirmYes : confirmYes;
    if (sel) {
      oled.setDrawColor(1);
      oled.drawBox(x, 44, 46, 15);
      oled.setDrawColor(0);
    } else {
      oled.setDrawColor(1);
      oled.drawFrame(x, 44, 46, 15);
    }
    oled.drawUTF8(x + (46 - oled.getUTF8Width(labels[i])) / 2, 55, labels[i]);
  }
  oled.setDrawColor(1);
}

void drawListMenu(const char* title, int maxItems, std::function<String(int)> getItemText,
                  const char* subtitle = nullptr) {
  drawHeader(title, nullptr, subtitle == nullptr);
  int rowOffset = 0;
  if (subtitle != nullptr) {
    // Podtytul (np. nazwa sieci) pod naglowkiem, mniejsza czcionka, wysrodkowany i przyciety.
    String sub = subtitle;
    bool ascii = true;
    for (size_t k = 0; k < sub.length(); k++) if ((uint8_t)sub[k] >= 0x80) ascii = false;
    // Najmniejsza czcionka dla nazw ASCII; nazwy z polskimi znakami - nieco wieksza, ale tez z diakrytykami
    oled.setFont(ascii ? u8g2_font_4x6_tr : u8g2_font_NokiaSmallPlain_te);
    while (sub.length() > 1 && oled.getUTF8Width(sub.c_str()) > 120) removeLastUtf8Char(sub);
    oled.drawUTF8((128 - oled.getUTF8Width(sub.c_str())) / 2, ascii ? 18 : 20, sub.c_str());
    oled.drawHLine(0, 23, 128);   // separator dopiero po nazwie sieci
    oled.setFont(u8g2_font_6x12_te);
    rowOffset = 13;   // wiekszy odstep pod separatorem
  }

  for (int i = 0; i < listVisibleRows(); i++) {
    int itemIdx = menuScroll + i;
    if (itemIdx >= maxItems) break;
    int yPos = 23 + rowOffset + (i * 12);

    if (itemIdx == menuIndex) {
      oled.setDrawColor(1);
      oled.drawBox(0, yPos - 9, 128, 12);
      oled.setDrawColor(0);
    } else {
      oled.setDrawColor(1);
    }

    String itemText = getItemText(itemIdx);
    int textWidth = oled.getUTF8Width(itemText.c_str());
    bool selectedSdFile = currentUi == UI_MENU_SD && itemIdx == menuIndex &&
                          !sdMenuEntries[itemIdx].isDirectory;
    bool selectedScrollable = itemIdx == menuIndex &&
                              (currentUi == UI_WIFI_SCAN || currentUi == UI_MENU_WEB);
    if ((selectedSdFile || selectedScrollable) && textWidth > 125) {
      int cycleWidth = textWidth + MARQUEE_GAP_PX;
      int offset = marqueeOffset(itemText, cycleWidth);
      int x = 3 - offset;
      oled.drawUTF8(x, yPos, itemText.c_str());
      oled.drawUTF8(x + cycleWidth, yPos, itemText.c_str());
    } else {
      oled.drawUTF8(3, yPos, itemText.c_str());
    }
  }
  oled.setDrawColor(1);
}

void drawMainMenuIcon(int index, int centerX, int centerY) {
  switch (index) {
    case 0:
      oled.drawHLine(centerX - 6, centerY, 13);
      oled.drawLine(centerX - 6, centerY, centerX, centerY - 6);
      oled.drawLine(centerX - 6, centerY, centerX, centerY + 6);
      break;
    case 1:
      oled.drawFrame(centerX - 6, centerY - 7, 12, 15);
      oled.drawLine(centerX + 1, centerY - 7, centerX + 6, centerY - 2);
      oled.drawHLine(centerX - 3, centerY - 2, 6);
      oled.drawHLine(centerX - 3, centerY + 1, 6);
      break;
    case 2:
      oled.drawFrame(centerX - 7, centerY - 3, 14, 11);
      oled.drawLine(centerX - 5, centerY - 3, centerX + 2, centerY - 7);
      oled.drawCircle(centerX - 3, centerY + 1, 2);
      oled.drawHLine(centerX + 1, centerY, 5);
      oled.drawHLine(centerX + 1, centerY + 3, 5);
      break;
    case 3:
      oled.drawCircle(centerX - 2, centerY - 2, 5);
      oled.drawLine(centerX + 2, centerY + 2, centerX + 7, centerY + 7);
      break;
    case 4:
      oled.drawCircle(centerX, centerY, 7);
      oled.drawLine(centerX, centerY, centerX, centerY - 4);
      oled.drawLine(centerX, centerY, centerX + 4, centerY + 2);
      break;
    case 5:
      oled.drawCircle(centerX, centerY, 7);
      oled.drawCircle(centerX, centerY - 4, 1);
      oled.drawVLine(centerX, centerY - 1, 7);
      break;
    case 6:   // kula ziemska (WWW)
      oled.drawCircle(centerX, centerY, 7);
      oled.drawEllipse(centerX, centerY, 3, 7, U8G2_DRAW_ALL);
      oled.drawHLine(centerX - 7, centerY, 15);
      break;
    case 7:   // zebatka (Ustawienia)
      oled.drawCircle(centerX, centerY, 4);
      oled.drawCircle(centerX, centerY, 1);
      oled.drawVLine(centerX, centerY - 7, 3);
      oled.drawVLine(centerX, centerY + 5, 3);
      oled.drawHLine(centerX - 7, centerY, 3);
      oled.drawHLine(centerX + 5, centerY, 3);
      oled.drawLine(centerX - 5, centerY - 5, centerX - 4, centerY - 4);
      oled.drawLine(centerX + 5, centerY - 5, centerX + 4, centerY - 4);
      oled.drawLine(centerX - 5, centerY + 5, centerX - 4, centerY + 4);
      oled.drawLine(centerX + 5, centerY + 5, centerX + 4, centerY + 4);
      break;
  }
}

void drawMainMenu() {
  // Siatka 3x2, kafelki 42x32 px.
  for (int i = 0; i < mainMenuCount; i++) {
    int row = i / 3;
    int col = i % 3;
    int cellX = 1 + col * 42;
    int cellY = row * 32;
    bool selected = i == menuIndex;

    if (selected) {
      oled.drawBox(cellX + 1, cellY + 1, 40, 30);
      oled.setDrawColor(0);
    } else {
      oled.drawFrame(cellX + 1, cellY + 1, 40, 30);
      oled.setDrawColor(1);
    }

    drawMainMenuIcon(mainMenuOrder[i], cellX + 21, cellY + 11);
    oled.setFont(u8g2_font_NokiaSmallPlain_te);
    int labelWidth = oled.getUTF8Width(mainMenuItems[i]);
    if (labelWidth > 40) {   // dlugi napis -> mniejsza czcionka, zeby miescil sie w kafelku
      oled.setFont(u8g2_font_5x7_tr);
      labelWidth = oled.getUTF8Width(mainMenuItems[i]);
    }
    oled.drawUTF8(cellX + (42 - labelWidth) / 2, cellY + 29, mainMenuItems[i]);
    oled.setDrawColor(1);
  }
}

// --- Dane do ekranu Informacje ---
uint64_t sdTotalBytes = 0;
uint64_t sdUsedBytes = 0;
bool sdInfoValid = false;

// Odczyt zajetosci karty (skanuje FAT - moze chwile potrwac, wiec robimy to tylko przy wejsciu w Informacje).
void sdInfoUpdate() {
  if (!sdReady) { sdInfoValid = false; return; }
  drawBusyScreen("Odczyt karty SD", "");
  sdBusyBegin();
  sdTotalBytes = SD.totalBytes();
  sdUsedBytes = SD.usedBytes();
  sdBusyEnd();
  sdInfoValid = sdTotalBytes > 0;
}

String formatBytes(uint64_t bytes) {
  char buf[16];
  if (bytes >= 1000000000ULL) snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1e9);
  else snprintf(buf, sizeof(buf), "%.0f MB", bytes / 1e6);
  return String(buf);
}

int fmRssiCached = -1;
uint32_t fmRssiAt = 0;
int readFmRssi() {
  if (!radioReady) return -1;
  if (fmRssiAt == 0 || millis() - fmRssiAt > 500) {
    si4703ReadRegisters();
    fmRssiCached = si4703Regs[0x0A] & 0xFF;   // RSSI w dBuV
    fmRssiAt = millis();
  }
  return fmRssiCached;
}

// Wiersz Informacji: zwraca true dla naglowka sekcji (wtedy 'label' = tytul), inaczej wypelnia etykiete i wartosc.
bool infoRow(int displayIndex, String &label, String &value) {
  int index = infoOrder[displayIndex];
  char buf[40];
  value = "";
  switch (index) {
    // --- Odtwarzanie ---
    case 0: label = "Odtwarzanie"; return true;
    case 1:
      label = "Źródło";
      if (currentSource == SOURCE_RADIO) value = radioMuted ? "Radio FM (mute)" : "Radio FM";
      else if (currentSource == SOURCE_SD) value = sdPaused ? "Karta SD (pauza)" : "Karta SD";
      else if (currentSource == SOURCE_WEB) value = webMuted ? "Internet (mute)" : "Internet";
      else value = "brak";
      break;
    case 2:
      label = "Teraz";
      if (currentSource == SOURCE_RADIO) {
        float frequency = radioFrequencyKHz / 1000.0f;
        const char* stationName = fmStationNameForFrequency(frequency);
        value = stationName[0] != '\0' ? String(stationName) : String(frequency, 1) + " MHz";
      } else if (currentSource == SOURCE_SD) value = displayTrackName(currentTrackName);
      else if (currentSource == SOURCE_WEB) value = webStations[webStationIndex].name;
      else value = "-";
      break;
    case 3:
      label = "Głośność";
      snprintf(buf, sizeof(buf), dacIsActiveSource() ? "%d" : "%d/15", activeVolumePercent());
      value = buf;
      break;
    case 4:
      label = "Prędkość";
      snprintf(buf, sizeof(buf), "%.1fx", playbackSpeed);
      value = buf;
      break;
    // --- System ---
    case 5: label = "System"; return true;
    case 6:
      label = "Bateria";
      snprintf(buf, sizeof(buf), "%.2f V", readBatteryVoltage());
      value = buf;
      break;
    case 7: {
      label = "CPU";
      int load = cpuLoadPercent();
      if (load < 0) snprintf(buf, sizeof(buf), "%u MHz", (unsigned)getCpuFrequencyMhz());
      else snprintf(buf, sizeof(buf), "%d%% (%u MHz)", load, (unsigned)getCpuFrequencyMhz());
      value = buf;
      break;
    }
    case 8: {
      label = "RAM";
      uint32_t total = ESP.getHeapSize();
      uint32_t used = total - ESP.getFreeHeap();
      snprintf(buf, sizeof(buf), "%u/%u kB (%u%%)", (unsigned)(used / 1024), (unsigned)(total / 1024),
               (unsigned)(total ? (uint64_t)used * 100 / total : 0));
      value = buf;
      break;
    }
    case 9: {
      label = "PSRAM";
      uint32_t total = ESP.getPsramSize();
      if (total == 0) { value = "brak"; break; }
      uint32_t used = total - ESP.getFreePsram();
      snprintf(buf, sizeof(buf), "%.1f/%.1f MB", used / 1048576.0f, total / 1048576.0f);
      value = buf;
      break;
    }
    // --- Karta SD ---
    case 10: label = "Karta SD"; return true;
    case 11: label = "Stan"; value = sdReady ? "gotowa" : "brak karty"; break;
    case 12:
      label = "Zajęte";
      if (sdReady && sdInfoValid) {
        value = formatBytes(sdUsedBytes) + " (" + String((unsigned)(sdUsedBytes * 100 / sdTotalBytes)) + "%)";
      } else value = "-";
      break;
    case 13:
      label = "Pojemność";
      value = (sdReady && sdInfoValid) ? formatBytes(sdTotalBytes) : String("-");
      break;
    // --- Siec ---
    case 14: label = "Sieć"; return true;
    case 15:
      label = "Wi-Fi";
      if (wifiIsConnected()) value = WiFi.SSID();
      else value = wifiSsid.length() > 0 ? wifiSsid + " (wył.)" : String("brak sieci");
      break;
    case 16:
      label = "IP";
      value = wifiIsConnected() ? WiFi.localIP().toString() : String("-");
      break;
    case 17:
      label = "Sygnał";
      if (wifiIsConnected()) {
        int rssi = WiFi.RSSI();
        snprintf(buf, sizeof(buf), "%d dBm (%d%%)", rssi, constrain(2 * (rssi + 100), 0, 100));
        value = buf;
      } else value = "-";
      break;
    // --- Radio FM ---
    case 18: label = "Radio FM"; return true;
    case 19: label = "Tuner"; value = radioReady ? "gotowy" : "brak"; break;
    case 20: {
      label = "Sygnał";
      int rssi = readFmRssi();
      if (rssi < 0) { value = "-"; break; }
      snprintf(buf, sizeof(buf), "%d dBµV", rssi);
      value = buf;
      break;
    }
    case 21:
      label = "RDS nazwa";
      if (currentSource != SOURCE_RADIO) value = "-";
      else value = rdsPsText.length() > 0 ? rdsPsText : String("czekam");
      break;
    case 22:
      label = "RDS typ";
      if (currentSource != SOURCE_RADIO) value = "-";
      else value = rdsPtyText.length() > 0 ? rdsPtyText : String("czekam");
      break;
    case 23:
      label = "RDS data";
      if (currentSource != SOURCE_RADIO) value = "-";
      else value = rdsCtDateText.length() > 0 ? rdsCtDateText : String("czekam");
      break;
    case 24:
      label = "RDS godzina";
      if (currentSource != SOURCE_RADIO) value = "-";
      else if (rdsCtTimeText.length() > 0) value = rdsCtTimeText;
      else value = rdsPsClockText.length() > 0 ? rdsPsClockText : String("czekam");
      break;
    // --- Czas ---
    case 25: label = "Czas"; return true;
    case 26:
      label = "Data";
      if (rtcReady) { snprintf(buf, sizeof(buf), "%02u.%02u.%04u", rtcDay, rtcMonth, rtcYear); value = buf; }
      else value = "brak zegara";
      break;
    case 27:
      label = "Godzina";
      if (rtcReady) { snprintf(buf, sizeof(buf), "%02u:%02u", rtcHour, rtcMinute); value = buf; }
      else value = "brak zegara";
      break;
    default: label = ""; break;
  }
  return false;
}

void drawInfoMenu() {
  drawHeader("Informacje");
  oled.setFont(u8g2_font_NokiaSmallPlain_te);

  for (int row = 0; row < infoVisibleRows; row++) {
    int itemIndex = menuScroll + row;
    if (itemIndex >= infoItemCount) break;

    int baseline = 22 + row * 9;
    String label, value;
    bool isHeader = infoRow(itemIndex, label, value);

    if (isHeader) {
      // Naglowek sekcji: wysrodkowany tytul z kreskami po bokach
      int w = oled.getUTF8Width(label.c_str());
      int x = (124 - w) / 2;
      oled.drawUTF8(x, baseline, label.c_str());
      oled.drawHLine(2, baseline - 3, max(0, x - 6));
      oled.drawHLine(x + w + 4, baseline - 3, max(0, 122 - (x + w + 4)));
      continue;
    }

    if (itemIndex == menuIndex) {
      oled.drawBox(0, baseline - 7, 124, 9);
      oled.setDrawColor(0);
    }

    // Etykieta po lewej, wartosc wyrownana do prawej
    oled.drawUTF8(3, baseline, label.c_str());
    int avail = 118 - oled.getUTF8Width(label.c_str()) - 4;
    while (value.length() > 1 && oled.getUTF8Width(value.c_str()) > avail) removeLastUtf8Char(value);
    oled.drawUTF8(121 - oled.getUTF8Width(value.c_str()), baseline, value.c_str());
    oled.setDrawColor(1);
  }

  const int trackY = 17;
  const int trackHeight = 45;
  int thumbHeight = (trackHeight * infoVisibleRows) / infoItemCount;
  int thumbRange = trackHeight - thumbHeight;
  int thumbY = trackY + (thumbRange * menuScroll) / (infoItemCount - infoVisibleRows);
  oled.drawVLine(127, trackY, trackHeight);
  oled.drawVLine(126, thumbY + 1, thumbHeight - 2);
}

void drawTextEntry() {
  oled.setFont(u8g2_font_6x12_te);
  oled.drawUTF8(2, 9, entryPurpose == ENTRY_SSID ? "Nazwa sieci" : "Hasło");
  int lw = oled.getUTF8Width(entrySetLabels[entrySet]);
  oled.drawUTF8(126 - lw, 9, entrySetLabels[entrySet]);
  oled.drawHLine(0, 12, 128);

  // Wpisany tekst (konczy sie kursorem; pokazujemy koncowke, jesli sie nie miesci).
  String shown = entryText;
  while (shown.length() > 0 && oled.getUTF8Width(shown.c_str()) > 116) shown.remove(0, 1);
  oled.drawUTF8(2, 27, shown.c_str());
  int cursorX = 2 + oled.getUTF8Width(shown.c_str());
  if ((millis() / 500) % 2 == 0) oled.drawHLine(cursorX + 1, 28, 5);

  // Pasek wyboru znaku: zaznaczony znak na srodku.
  int n = entrySetLength(entrySet);
  const int slots = 9;
  const int slotW = 14;
  const int x0 = (128 - slots * slotW) / 2;
  for (int slot = 0; slot < slots; slot++) {
    int offset = slot - slots / 2;
    int idx = entryIdx + offset;
    if (idx < 0 || idx >= n) continue;

    String label = entryLabel(entrySet, idx);
    int w = oled.getUTF8Width(label.c_str());
    int cx = x0 + slot * slotW + slotW / 2;
    if (offset == 0) {
      oled.drawBox(cx - 7, 33, 14, 14);
      oled.setDrawColor(0);
    }
    oled.drawUTF8(cx - w / 2, 44, label.c_str());
    oled.setDrawColor(1);
  }

  oled.setFont(u8g2_font_NokiaSmallPlain_te);
  oled.drawUTF8(2, 61, "E1:zestaw E2:wstaw B3:wyjdź");
}

void drawUiMessage() {
  if (uiMessageUntil == 0) return;
  if ((int32_t)(millis() - uiMessageUntil) >= 0) {
    uiMessageUntil = 0;
    return;
  }

  oled.setDrawColor(1);
  oled.drawBox(6, 14, 116, 36);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_6x12_te);

  String l1 = uiMsg1;
  while (l1.length() > 3 && oled.getUTF8Width(l1.c_str()) > 108) removeLastUtf8Char(l1);
  oled.drawUTF8((128 - oled.getUTF8Width(l1.c_str())) / 2, 30, l1.c_str());

  if (uiMsg2.length() > 0) {
    String l2 = uiMsg2;
    while (l2.length() > 3 && oled.getUTF8Width(l2.c_str()) > 108) removeLastUtf8Char(l2);
    oled.drawUTF8((128 - oled.getUTF8Width(l2.c_str())) / 2, 44, l2.c_str());
  }
  oled.setDrawColor(1);
}

// Ekrany Wi-Fi / radia internetowego oraz komunikat nakladany na dowolny ekran.
void drawExtraUi() {
  if (currentUi == UI_MENU_WEB) {
    drawListMenu("Radio internetowe", webStationCount, [](int i) -> String {
      return String(i + 1) + ". " + webStations[i].name;
    });
  } else if (currentUi == UI_MENU_SETTINGS) {
    drawListMenu("Ustawienia", settingsItemCount, [](int i) -> String {
      return String(settingsItems[i]);
    });
  } else if (currentUi == UI_MENU_SOUND) {
    drawListMenu("Korektor", SOUND_ITEM_COUNT, [](int i) -> String { return soundItemText(i); });
  } else if (currentUi == UI_MENU_DISPLAY) {
    drawListMenu("Wyświetlacz", displayModeCount, [](int i) -> String {
      return String((i == (lightMode ? 1 : 0)) ? "* " : "  ") + displayModeItems[i];
    });
  } else if (currentUi == UI_MENU_WIFI) {
    String ssidText = (wifiSsid.length() > 0 ? wifiSsid : String("brak sieci"));
    drawListMenu("Wi-Fi", wifiMenuItemCount, [](int i) -> String {
      return String(wifiMenuItems[i]);
    }, ssidText.c_str());
  } else if (currentUi == UI_WIFI_SCAN) {
    drawListMenu("Sieci Wi-Fi", wifiNetCount, [](int i) -> String {
      return String(wifiNets[i].open ? "  " : "* ") + wifiNets[i].ssid;
    });
  } else if (currentUi == UI_TEXT_ENTRY) {
    drawTextEntry();
  }
  drawUiMessage();
}

void oledUpdate(bool force) {
  if (!oledReady) return;
  // Podczas przewijania tekstu odswiezamy ekran duzo czesciej (plynny ruch).
  uint32_t updateInterval = marqueeActive ? MARQUEE_FRAME_MS
                          : (batteryVoltageCached < LOW_BATTERY_WARNING_VOLTAGE ? 200 : 400);
  if (!force && millis() - lastOledUpdate < updateInterval) return;
  lastOledUpdate = millis();

  marqueeActive = false;   // ustawi sie ponownie, jesli ta klatka przewija tekst
  oled.clearBuffer();

  if (currentUi == UI_PLAYER) {
    drawStatusBar();

    // ------- WIDOK RADIA -------
    if (currentSource == SOURCE_RADIO) {
      // Czestotliwosc (nieco mniejsza), pod nia nazwa stacji, w lewym dolnym rogu zawsze "Stacja x/y".
      float freqNow = si4703GetFrequencyMHz();
      oled.setFont(u8g2_font_logisoso20_tn);
      char freqT[8]; snprintf(freqT, sizeof(freqT), "%.1f", freqNow);
      int fw = oled.getUTF8Width(freqT);
      int sx = (128 - fw - 18) / 2;
      oled.drawUTF8(sx, 38, freqT);
      oled.setFont(u8g2_font_6x12_te);
      oled.drawUTF8(sx + fw + 3, 38, "MHz");

      // Nazwa stacji (z listy zapisanych albo z RDS), wysrodkowana pod czestotliwoscia
      String nameLine;
      if (radioMuted) nameLine = "WYCISZONE";
      else if (!radioReady) nameLine = "Brak Si4703";
      else {
        nameLine = fmStationNameForFrequency(freqNow);
        if (nameLine.length() == 0) { nameLine = rdsPsText; nameLine.trim(); }
      }
      oled.setFont(u8g2_font_6x12_te);
      while (nameLine.length() > 1 && oled.getUTF8Width(nameLine.c_str()) > 124) removeLastUtf8Char(nameLine);
      oled.drawUTF8((128 - oled.getUTF8Width(nameLine.c_str())) / 2, 49, nameLine.c_str());

      oled.drawHLine(0, 52, 128);
      oled.setFont(u8g2_font_NokiaSmallPlain_te);
      int posIdx = -1;   // numer stacji na liscie zapisanych (jesli aktualna czestotliwosc jest na liscie)
      for (int i = 0; i < fmStationCount; i++) {
        if (fabsf(fmStations[i] - freqNow) < 0.05f) { posIdx = i; break; }
      }
      char stInfo[24];
      if (posIdx >= 0) snprintf(stInfo, sizeof(stInfo), "Stacja %d/%d", posIdx + 1, fmStationCount);
      else snprintf(stInfo, sizeof(stInfo), "Stacja -/%d", fmStationCount);
      oled.drawUTF8(2, 62, stInfo);

      drawMenuHint();
    }
    else if (currentSource == SOURCE_SD) {
      oled.setFont(u8g2_font_helvB10_te);
      String trackTitle = displayTrackName(currentTrackName);
      int titleWidth = oled.getUTF8Width(trackTitle.c_str());
      if (titleWidth <= 124) {
        oled.drawUTF8((128 - titleWidth) / 2, 30, trackTitle.c_str());
      } else {
        int cycleWidth = titleWidth + MARQUEE_GAP_PX;
        int offset = marqueeOffset(trackTitle, cycleWidth);
        int x = 2 - offset;
        oled.drawUTF8(x, 30, trackTitle.c_str());
        oled.drawUTF8(x + cycleWidth, 30, trackTitle.c_str());
      }

      // Pasek postepu, po jego prawej stronie czas (mala czcionka)
      uint32_t cur = audio.getAudioCurrentTime();
      uint32_t dur = audio.getAudioFileDuration();
      char timeText[24];
      if (dur > 0) snprintf(timeText, sizeof(timeText), "%u:%02u/%u:%02u",
                            (unsigned)(cur / 60), (unsigned)(cur % 60), (unsigned)(dur / 60), (unsigned)(dur % 60));
      else snprintf(timeText, sizeof(timeText), "%u:%02u", (unsigned)(cur / 60), (unsigned)(cur % 60));
      oled.setFont(u8g2_font_5x7_tr);
      int timeW = oled.getUTF8Width(timeText);
      oled.drawUTF8(128 - timeW - 2, 43, timeText);

      int barW = 128 - timeW - 2 - 4 - 4;   // lewy margines 4, odstep od czasu 4
      oled.drawFrame(4, 37, barW, 7);
      if (dur > 0) {
        int fill = (int)((uint64_t)min(cur, dur) * (barW - 4) / dur);
        if (fill > 0) oled.drawBox(6, 39, fill, 3);
      }
      oled.setFont(u8g2_font_NokiaSmallPlain_te);

      oled.drawHLine(0, 52, 128);
      // Symbol po lewej rysowany osobno - tekst stoi zawsze w tym samym miejscu.
      if (!sdPaused) {
        if ((millis() / 500) % 2 == 0) oled.drawTriangle(2, 54, 2, 62, 8, 58);
        oled.drawUTF8(12, 62, "ODTWARZANIE");
      } else {
        oled.drawBox(2, 54, 2, 8);
        oled.drawBox(6, 54, 2, 8);
        oled.drawUTF8(12, 62, "PAUZA");
      }
      drawMenuHint();
    }
    // ------- WIDOK RADIA INTERNETOWEGO -------
    else if (currentSource == SOURCE_WEB) {
      oled.setFont(u8g2_font_NokiaSmallPlain_te);

      oled.setFont(u8g2_font_helvB10_te);
      int shownIdx = (webPendingIdx >= 0) ? webPendingIdx : webStationIndex;
      String stationName = webStations[shownIdx].name;
      int nameWidth = oled.getUTF8Width(stationName.c_str());
      if (nameWidth <= 124) {
        oled.drawUTF8((128 - nameWidth) / 2, 34, stationName.c_str());
      } else {
        int cycleWidth = nameWidth + MARQUEE_GAP_PX;
        int x = 2 - marqueeOffset(stationName, cycleWidth, 0);
        oled.drawUTF8(x, 34, stationName.c_str());
        oled.drawUTF8(x + cycleWidth, 34, stationName.c_str());
      }

      oled.setFont(u8g2_font_NokiaSmallPlain_te);
      if (webTitle.length() > 0) {
        int titleWidth = oled.getUTF8Width(webTitle.c_str());
        if (titleWidth <= 124) {
          oled.drawUTF8((128 - titleWidth) / 2, 47, webTitle.c_str());
        } else {
          int cycleWidth = titleWidth + MARQUEE_GAP_PX;
          int x = 2 - marqueeOffset(webTitle, cycleWidth, 1);
          oled.drawUTF8(x, 47, webTitle.c_str());
          oled.drawUTF8(x + cycleWidth, 47, webTitle.c_str());
        }
      }

      oled.drawHLine(0, 52, 128);
      const char* statusText = (webPendingIdx >= 0) ? "Zmiana stacji..."
                             : webMuted ? "WYCISZONE"
                             : (webReconnectAt != 0) ? "Ponawiam połączenie..."
                             : (millis() - webStartMs < 3000) ? "Buforowanie..."
                             : "NA ŻYWO";
      if (recActive && webPendingIdx < 0) {
        // Nagrywanie: migajaca kropka + czas + rozmiar pliku
        uint32_t sec = (millis() - recStartMs) / 1000;
        char recText[28];
        snprintf(recText, sizeof(recText), "REC %u:%02u %.1f MB", (unsigned)(sec / 60), (unsigned)(sec % 60), recBytes / 1048576.0f);
        if ((millis() / 500) % 2 == 0) oled.drawDisc(5, 58, 3);
        oled.drawUTF8(12, 62, recText);
      } else {
        oled.drawUTF8(2, 62, statusText);
      }
      drawMenuHint();
    }
    // ------- WIDOK STANDBY -------
    else {
      char dateStr[20];
      snprintf(dateStr, sizeof(dateStr), "%02d.%02d.%04d", rtcDay, rtcMonth, rtcYear);
      char clockStr[6];
      snprintf(clockStr, sizeof(clockStr), "%02d:%02d", rtcHour, rtcMinute);

      oled.setFont(u8g2_font_logisoso22_tn);
      int clockW = oled.getUTF8Width(clockStr);
      oled.drawUTF8((128 - clockW) / 2, 38, clockStr);

      oled.setFont(u8g2_font_helvB10_te);
      int dateW = oled.getUTF8Width(dateStr);
      oled.drawUTF8((128 - dateW) / 2, 59, dateStr);
      drawBatteryVoltage();
    }

    // Okienko glosnosci rysowane na wierzchu - dolna belka i gorny pasek zostaja widoczne.
    if (volumeOverlayVisible) {
      if ((int32_t)(millis() - volumeOverlayUntil) < 0) {
        oled.setDrawColor(1);
        oled.drawBox(22, 19, 84, 29);

        oled.setDrawColor(0);
        oled.setFont(u8g2_font_6x12_te);
        const char* volTitle = "GŁOŚNOŚĆ";
        int titleW = oled.getUTF8Width(volTitle);
        oled.drawUTF8((128 - titleW) / 2, 30, volTitle);

        char volText[12];
        snprintf(volText, sizeof(volText), "%d", volumeOverlayValue);
        oled.setFont(u8g2_font_helvB10_te);
        int volW = oled.getUTF8Width(volText);
        oled.drawUTF8((128 - volW) / 2, 44, volText);

        oled.setDrawColor(1);
      } else {
        volumeOverlayVisible = false;
      }
    }
  }
  // ------- WIDOKI MENU -------
  else if (currentUi == UI_MENU_MAIN) {
    drawMainMenu();
  }
  else if (currentUi == UI_MENU_DATETIME) {
    char timeText[6];
    char dateText[11];
    snprintf(timeText, sizeof(timeText), "%02u:%02u", editHour, editMinute);
    snprintf(dateText, sizeof(dateText), "%02u.%02u.%04u", editDay, editMonth, editYear);

    const char* fieldNames[] = { "GODZINA", "MINUTA", "DZIEŃ", "MIESIĄC", "ROK" };
    drawHeader("Data i godzina");
    oled.setFont(u8g2_font_helvB10_te);
    int timeWidth = oled.getUTF8Width(timeText);
    oled.drawUTF8((128 - timeWidth) / 2, 31, timeText);
    oled.drawUTF8(25, 48, dateText);
    oled.setFont(u8g2_font_NokiaSmallPlain_te);
    oled.drawUTF8(2, 61, "Pole:");
    oled.drawUTF8(29, 61, fieldNames[datetimeEditField]);
    const char* saveStatus = dateTimeSaved ? "Zapisano" : "Zapisz";
    drawBracketText(saveStatus, 61);
  }
  else if (currentUi == UI_MENU_INFO) {
    drawInfoMenu();
  }
  else if (currentUi == UI_MENU_SD) {
    if (sdMenuEntryCount == 0) {
      drawHeader("Karta SD");
      oled.drawUTF8(5, 30, "Pusty katalog");
      oled.drawUTF8(5, 42, "brak MP3.");
    } else {
      String title = (currentSdPath == "/") ? "Karta SD" : sdEntryLabel(currentSdPath);
      drawListMenu(title.c_str(), sdMenuEntryCount, [](int i) -> String {
        if (sdMenuEntries[i].isDirectory) return String("[K] ") + sdMenuEntries[i].name;
        int num = 1;   // numeracja utworow w katalogu (katalogow sie nie numeruje)
        for (int k = 0; k < i; k++) if (!sdMenuEntries[k].isDirectory) num++;
        return String(num) + ". " + displayTrackName(sdMenuEntries[i].name);
      });
    }
  }
  else if (currentUi == UI_MENU_RADIO) {
    if (fmStationCount == 0) {
      drawHeader("Radio FM");
      oled.drawUTF8(5, 30, "Brak zapisanych stacji.");
      oled.drawUTF8(5, 42, "Uruchom skanowanie w");
      oled.drawUTF8(5, 54, "Menu Głównym.");
    } else {
      drawListMenu("Radio FM", fmStationCount, [](int i) {
        String item = String(fmStations[i], 1);
        if (fmStationNames[i].length() > 0) item += " " + fmStationNames[i];
        return item;
      });
    }
  }
  else if (currentUi == UI_CONFIRM) {
    drawConfirm();
  }
  else if (currentUi == UI_SD_CLEAN) {
    drawSdClean();
  }
  drawExtraUi();
  oledSend();
}

// ================== SETUP / LOOP ==================

void setup() {
  setCpuFrequencyMhz(240);   // maksymalna wydajnosc - energooszczednosc nie jest priorytetem
  Serial.begin(115200);
  cpuMonitorInit();   // zawiera 300 ms kalibracji
  audioSettingsLoad();   // zapamietane glosnosci (FM / SD / WWW) i korektor
  Serial.println(F("=== ESP32-S3: Radio FM + SD MP3 Player ==="));
  Serial.println(F("Akumulator: 18650 Li-Ion, pomiar GPIO3 przez dzielnik 1:1"));

  encoderInit(enc1);
  encoderInit(enc2);
  encoderStartTask();   // odczyt pokretel w osobnym watku (nie gubi obrotow przy slabym Wi-Fi)
  buttonsInit();

  batteryInit();
  updateBatteryMeasurement(true);
  loadPresetStations();
  displayLoadSettings();
  wifiLoadCredentials();   // zapisana siec Wi-Fi (Wi-Fi wlaczamy dopiero dla radia internetowego)

  pinMode(MUX_SELECT_PIN, OUTPUT);
  digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_SD);

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);

  pinMode(OLED_CS_PIN, OUTPUT);
  digitalWrite(OLED_CS_PIN, HIGH);

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  if (!SD.begin(SD_CS_PIN, SPI)) {
    sdReady = false;
    Serial.println(F("BLAD: Nie wykryto karty SD!"));
  } else {
    sdReady = true;
    Serial.println(F("Karta SD gotowa."));
  }

  const bool resumeWeb = (startSource == 3 && wifiSsid.length() > 0);
  if (si4703Begin()) {
    if (startFmKHz >= RADIO_MIN_KHZ && startFmKHz <= RADIO_MAX_KHZ) {
      // Start od ostatnio sluchanej stacji FM
      currentStationIndex = -1;   // dopasuje sie przy pierwszym przelaczeniu
      setRadioFrequencyKHz(startFmKHz);
    } else {
      currentStationIndex = 0;
      setRadioFrequencyKHz((uint32_t)(fmStations[currentStationIndex] * 1000.0f + 0.5f));
    }
    currentSource = SOURCE_RADIO;
    if (resumeWeb) {
      // Ostatnio byl Web: FM zostaje cicho na czas laczenia z Wi-Fi i stacja
      si4703SetMute(true);
      digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_SD);
    } else {
      digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
      radioMuted = false;
      si4703SetMute(false);
    }
  }

  // Audio (MP3 z karty SD): NIE inicjalizujemy I2S tutaj - patrz ensureAudioInit().
  // Zegar I2S rusza dopiero przy pierwszym "play", zeby podczas samego
  // sluchania radia magistrala do PCM5102 byla calkowicie cicha.
  applyVolume();

  // DS3231 korzysta z tej samej magistrali I2C co Si4703
  rtcReady = ds3231Begin();

  oledInit();
  oledUpdate(true);

  // Zabezpieczenia po wczytaniu zapamietanych ustawien
  if (webStationIndex < 0 || webStationIndex >= webStationCount) webStationIndex = 0;
  if (lastMainTile < 0 || lastMainTile >= mainMenuCount) lastMainTile = 0;

  // Jesli ostatnio sluchano radia WWW - wznow ostatnia stacje
  if (resumeWeb && !startWebStation(webStationIndex) && radioReady && currentSource == SOURCE_RADIO) {
    // Nie udalo sie wznowic Web - wracamy do FM
    digitalWrite(MUX_SELECT_PIN, MUX_SOURCE_RADIO);
    radioMuted = false;
    si4703SetMute(false);
  }

  printHelp();
  Serial.print("> ");
}

void loop() {
  handleEncoders();
  handleButtons();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdBuffer.length() > 0) {
        Serial.println();
        handleCommand(cmdBuffer);
        cmdBuffer = "";
        Serial.print("> ");
      }
    } else if (c == '\b' || c == 127) {
      if (cmdBuffer.length() > 0) {
        cmdBuffer.remove(cmdBuffer.length() - 1);
      }
    } else {
      cmdBuffer += c;
    }
  }

  if (rtcReady && millis() - lastRtcRead >= 40000) {
    lastRtcRead = millis();
    if (!ds3231ReadTime()) {
      rtcReady = false;
      Serial.println(F("BLAD: Utracono komunikacje z DS3231."));
    }
  }

  if (currentSource == SOURCE_SD || currentSource == SOURCE_WEB) {
    audio.loop();
  } else if (currentSource == SOURCE_RADIO) {
    si4703UpdateRds();
  }

  handleSdEof();
  webService();
  recorderService();
  audioSettingsService();
  updateBatteryMeasurement();
  oledUpdate();
  delay(1);   // oddaje procesor watkowi bezczynnosci (pomiar CPU, mniejszy pobor pradu)
}