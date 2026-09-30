/*
  ===========================================================================
  IrigasiPot — Firmware ESP8266
  ===========================================================================
  Sistem irigasi otomatis solar-powered:
    - Mode AP+STA permanen: dashboard di-self-host di ESP8266 sendiri,
      diakses lewat hotspot "IrigasiPot-Setup" (192.168.4.1), sekaligus
      tetap konek ke WiFi rumah/sekolah untuk internet (NTP + ntfy).
    - Manajemen banyak jaringan WiFi tersimpan lewat wifi_manager.html.
    - Penjadwalan siram (harian, per jam:menit) lewat dashboard.html.
    - Notifikasi push ke HP via ntfy.sh setiap selesai siklus siram.
    - Deep sleep presisi (dipecah per siklus max ~55 menit) untuk hemat
      baterai, bangun lewat D0->RST (hardware) atau tombol manual.

  Board   : NodeMCU ESP8266 v3
  Storage : LittleFS  (halaman web statis + file konfigurasi JSON)
  Relay   : D7 / GPIO13, AKTIF LOW (ON=LOW, OFF=HIGH)
  Wake    : D0 -> RST (hardware, lihat panduan_sambungan_kabel.md Bagian 3)
            + tombol manual RST-GND

  Library yang perlu di-install lewat Library Manager Arduino IDE:
    - ArduinoJson      (Benoit Blanchon)  v6.x
    - NTPClient        (Fabrice Weinberg)
  Ikut otomatis saat install ESP8266 Board Package (tidak perlu install manual):
    - ESP8266WiFi, ESP8266WebServer, ESP8266HTTPClient, WiFiClientSecure, LittleFS

  Sebelum upload:
    1. Pastikan file di folder data/ (wifi_manager.html, dashboard.html)
       di-upload ke LittleFS lewat "ESP8266 LittleFS Data Upload" tool
       (Arduino IDE 1.8.x) atau Filesystem uploader di IDE 2.x/arduino-cli.
    2. Sesuaikan UTC_OFFSET_SECONDS di bawah kalau device dipakai di luar WIB.
    3. cfg.maxDuration & ntfyUrl diatur lewat dashboard (halaman Pengaturan),
       tidak perlu di-hardcode di sini.
  ===========================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <vector>
#include <time.h>

// ---------------------------------------------------------------------
// KONFIGURASI — sesuaikan di sini kalau perlu
// ---------------------------------------------------------------------
#define RELAY_PIN 13          // D7 pada NodeMCU (lihat panduan_sambungan_kabel.md langkah 11)
#define RELAY_ON  LOW         // Relay module aktif-LOW: D7 LOW = ON
#define RELAY_OFF HIGH        // Logika OFF = HIGH
// Untuk kondisi OFF, lepaskan D7 dari mode OUTPUT dan tahan HIGH
// dengan pull-up internal. Ini mengurangi kemungkinan input relay
// tertarik LOW oleh rangkaian internal/modul.
#define RELAY_OFF_USE_INPUT_PULLUP true

// Mode troubleshooting. Biarkan true selama pengujian relay.
#define DEBUG_RELAY            true
#define DEBUG_SCHEDULE         true
#define DEBUG_SERIAL_COMMANDS  true

// Scheduler sengaja ditahan beberapa detik setelah boot agar kita bisa
// membedakan relay ON akibat startup/wiring dengan relay ON akibat jadwal.
static const unsigned long STARTUP_SCHEDULER_GUARD_MS = 15000UL;

// Waktu kecil untuk memberi kesempatan GPIO benar-benar berada pada state OFF
// setelah pin dikonfigurasi. Ini bukan pengganti hardware pull-up/pull-down;
// hanya untuk membantu pengujian firmware.
static const unsigned long RELAY_BOOT_SETTLE_MS = 100UL;

static const char* AP_SSID     = "IrigasiPot-Setup";
static const char* AP_PASSWORD = "irigasi123";   // "" = open network. Isi min. 8 karakter kalau mau diproteksi.

static const long  UTC_OFFSET_SECONDS      = 7 * 3600;  // WIB (UTC+7) — ganti kalau device dipakai di zona lain
static const unsigned long IDLE_TIMEOUT_MS        = 5UL * 60UL * 1000UL; // 5 menit idle -> deep sleep
static const unsigned long SLEEP_CHUNK_SEC         = 55UL * 60UL;        // maks per siklus tidur (aman di bawah batas ~71 menit ESP.deepSleep)
static const unsigned long NO_TIME_FALLBACK_SEC    = 10UL * 60UL;        // fallback tidur kalau NTP belum sinkron
static const unsigned long SCHEDULE_TOLERANCE_SEC  = 5UL * 60UL;         // toleransi telat bangun vs jam jadwal
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000UL;             // batas waktu coba tiap jaringan tersimpan
static const unsigned long NTP_SYNC_TIMEOUT_MS     = 8000UL;
static const unsigned long NTP_RETRY_INTERVAL_MS   = 15000UL; // retry NTP jika Wi-Fi tersambung setelah boot
static const int   DEFAULT_MAX_DURATION_SEC = 60;        // batas dry-run protection default (detik)
static const int   MANUAL_WATER_DEFAULT_SEC = 8;         // durasi default tombol "Siram sekarang"

static const char* WIFI_FILE     = "/wifi.json";
static const char* SCHEDULE_FILE = "/schedule.json";
static const char* CONFIG_FILE   = "/config.json";
static const char* STATE_FILE    = "/state.json";

// ---------------------------------------------------------------------
// STATE GLOBAL
// ---------------------------------------------------------------------
struct WifiEntry     { String ssid; String password; };
struct ScheduleSlot   { String time; int duration; }; // time = "HH:MM", duration = detik

std::vector<WifiEntry>     wifiList;
std::vector<ScheduleSlot>  schedule_;

struct Config {
  String ntfyUrl;
  int maxDuration = DEFAULT_MAX_DURATION_SEC;
  int manualWaterDuration = MANUAL_WATER_DEFAULT_SEC;
} cfg;

struct DeviceState {
  time_t lastWateredEpoch = 0;
  String lastScheduleKey = ""; // YYYY-MM-DD HH:MM, slot terjadwal terakhir yang sudah dipicu
} devState;

ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", UTC_OFFSET_SECONDS, 60000);

bool timeSynced = false;
bool pumpOn = false;
unsigned long pumpStartMs = 0;
unsigned long pumpDurationMs = 0;
unsigned long lastActivityMs = 0;
unsigned long lastScheduleCheckMs = 0;
unsigned long lastNtpRetryMs = 0;
unsigned long bootMs = 0;
unsigned long relayOnCount = 0;
unsigned long relayOffCount = 0;
unsigned long lastRelayMonitorMs = 0;
int relayLastCommandLevel = RELAY_OFF;
String relayLastReason = "belum ada";
bool relayManualOverride = false;

// ---------------------------------------------------------------------
// UTIL — FILESYSTEM & JSON
// ---------------------------------------------------------------------
bool writeJsonFile(const char* path, const JsonDocument& doc) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool readJsonFile(const char* path, DynamicJsonDocument& doc) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

// ---------------------------------------------------------------------
// WIFI LIST (dipakai oleh wifi_manager.html)
// ---------------------------------------------------------------------
void loadWifiList() {
  wifiList.clear();
  DynamicJsonDocument doc(4096);
  if (!readJsonFile(WIFI_FILE, doc)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    WifiEntry e;
    e.ssid = String((const char*)(o["ssid"] | ""));
    e.password = String((const char*)(o["password"] | ""));
    if (e.ssid.length()) wifiList.push_back(e);
  }
}

void saveWifiList() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &e : wifiList) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = e.ssid;
    o["password"] = e.password;
  }
  writeJsonFile(WIFI_FILE, doc);
}

int findWifiIndex(const String& ssid) {
  for (size_t i = 0; i < wifiList.size(); i++)
    if (wifiList[i].ssid == ssid) return (int)i;
  return -1;
}

// ---------------------------------------------------------------------
// SCHEDULE
// ---------------------------------------------------------------------
void loadSchedule() {
  schedule_.clear();
  DynamicJsonDocument doc(4096);
  if (!readJsonFile(SCHEDULE_FILE, doc)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    ScheduleSlot s;
    s.time = String((const char*)(o["time"] | "12:00"));
    s.duration = o["duration"] | 8;
    schedule_.push_back(s);
  }
}

void saveSchedule() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &s : schedule_) {
    JsonObject o = arr.createNestedObject();
    o["time"] = s.time;
    o["duration"] = s.duration;
  }
  writeJsonFile(SCHEDULE_FILE, doc);
}

// ---------------------------------------------------------------------
// CONFIG & STATE
// ---------------------------------------------------------------------
void loadConfig() {
  DynamicJsonDocument doc(512);
  if (readJsonFile(CONFIG_FILE, doc)) {
    cfg.ntfyUrl = normalizeNtfyUrl(String((const char*)(doc["ntfyUrl"] | "")));
    cfg.maxDuration = constrain((int)(doc["maxDuration"] | DEFAULT_MAX_DURATION_SEC), 1, 600);
    cfg.manualWaterDuration = constrain((int)(doc["manualWaterDuration"] | MANUAL_WATER_DEFAULT_SEC), 1, cfg.maxDuration);
  }
}

void saveConfig() {
  DynamicJsonDocument doc(512);
  doc["ntfyUrl"] = cfg.ntfyUrl;
  doc["maxDuration"] = cfg.maxDuration;
  doc["manualWaterDuration"] = cfg.manualWaterDuration;
  writeJsonFile(CONFIG_FILE, doc);
}

void loadState() {
  DynamicJsonDocument doc(384);
  if (readJsonFile(STATE_FILE, doc)) {
    devState.lastWateredEpoch = (time_t)(long)(doc["lastWateredEpoch"] | 0);
    devState.lastScheduleKey = String((const char*)(doc["lastScheduleKey"] | ""));
  }
}

void saveState() {
  DynamicJsonDocument doc(384);
  doc["lastWateredEpoch"] = (long)devState.lastWateredEpoch;
  doc["lastScheduleKey"] = devState.lastScheduleKey;
  writeJsonFile(STATE_FILE, doc);
}

// ---------------------------------------------------------------------
// WAKTU
//
// Catatan: NTPClient di-init dengan UTC_OFFSET_SECONDS, jadi nowEpoch()
// mengembalikan "epoch yang sudah digeser ke waktu lokal (WIB)". Nilai
// ini dipakai KONSISTEN ke seluruh firmware (gmtime_r/mktime memperlakukan
// nilai ini seolah UTC murni) — ini trik umum untuk device tanpa
// database timezone, dan valid karena hanya dipakai secara internal.
// ---------------------------------------------------------------------
time_t nowEpoch() {
  return (time_t)timeClient.getEpochTime();
}

bool syncTime() {
  timeClient.begin();
  unsigned long start = millis();
  timeClient.forceUpdate();
  while (!timeClient.isTimeSet() && millis() - start < NTP_SYNC_TIMEOUT_MS) {
    timeClient.update();
    delay(200);
  }
  return timeClient.isTimeSet();
}

void maintainTimeSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (timeSynced) return;

  if (lastNtpRetryMs != 0 && millis() - lastNtpRetryMs < NTP_RETRY_INTERVAL_MS) {
    return;
  }

  lastNtpRetryMs = millis();
  Serial.println("[NTP] Mencoba sinkron ulang karena Wi-Fi sudah terhubung...");
  timeSynced = syncTime();

  if (timeSynced) {
    time_t now = nowEpoch();
    struct tm tn;
    gmtime_r(&now, &tn);
    Serial.printf("[NTP] Waktu tersinkron ulang: %04d-%02d-%02d %02d:%02d:%02d\n",
                  tn.tm_year + 1900, tn.tm_mon + 1, tn.tm_mday,
                  tn.tm_hour, tn.tm_min, tn.tm_sec);
  } else {
    Serial.println("[NTP] Sinkron ulang gagal, akan dicoba lagi.");
  }
}

String formatLastWatered(time_t epoch) {
  if (epoch == 0) return "-";
  time_t now = nowEpoch();
  struct tm tmW, tmN;
  gmtime_r(&epoch, &tmW);
  gmtime_r(&now, &tmN);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", tmW.tm_hour, tmW.tm_min);
  bool sameDay = (tmW.tm_year == tmN.tm_year && tmW.tm_yday == tmN.tm_yday);
  String out = sameDay ? "Hari ini, " : "Kemarin, ";
  out += buf;
  return out;
}

// Epoch untuk waktu "HH:MM" pada tanggal yang sama dengan `ref`
time_t epochForTimeToday(time_t ref, const String& hhmm) {
  int hh = hhmm.substring(0, 2).toInt();
  int mm = hhmm.substring(3, 5).toInt();
  struct tm t;
  gmtime_r(&ref, &t);
  t.tm_hour = hh; t.tm_min = mm; t.tm_sec = 0;
  return mktime(&t);
}

// Jadwal berikutnya (epoch), lompat ke besok kalau semua slot hari ini sudah lewat
time_t nextScheduleEpoch(time_t ref) {
  if (schedule_.empty()) return 0;
  time_t best = 0;
  for (auto &s : schedule_) {
    time_t e = epochForTimeToday(ref, s.time);
    if (e <= ref) e += 24UL * 3600UL;
    if (best == 0 || e < best) best = e;
  }
  return best;
}

String normalizeNtfyUrl(String raw) {
  raw.trim();
  if (!raw.length()) return "";

  if (raw.startsWith("http://") || raw.startsWith("https://")) {
    return raw;
  }

  // Izinkan input singkat:
  //   ntfy.sh/topic-kamu
  //   topic-kamu          -> https://ntfy.sh/topic-kamu
  //   myserver/topic      -> https://myserver/topic
  if (raw.startsWith("ntfy.sh/")) {
    return "https://" + raw;
  }

  if (raw.indexOf('/') < 0) {
    return "https://ntfy.sh/" + raw;
  }

  return "https://" + raw;
}

// ---------------------------------------------------------------------
// NTFY — notifikasi push
// ---------------------------------------------------------------------
bool sendNtfy(const String& message) {
  if (cfg.ntfyUrl.length() == 0) {
    Serial.println("[NTFY] Tidak dikirim: URL ntfy masih kosong.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[NTFY] Tidak dikirim: WiFi tidak terhubung. URL=%s\n", cfg.ntfyUrl.c_str());
    return false;
  }

  Serial.printf("[NTFY] Mengirim -> %s | pesan=\"%s\"\n",
                cfg.ntfyUrl.c_str(), message.c_str());

  HTTPClient http;
  int httpCode = -1;

  if (cfg.ntfyUrl.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure(); // ntfy.sh HTTPS; cocok untuk troubleshooting ESP8266
    http.setTimeout(2500);
    if (!http.begin(client, cfg.ntfyUrl)) {
      Serial.println("[NTFY] HTTPS begin() gagal.");
      return false;
    }

    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    http.addHeader("Title", "Irigasi Pot");
    http.addHeader("User-Agent", "IrigasiPot/1.0 ESP8266");

    httpCode = http.POST(message);
  } else {
    WiFiClient client;
    http.setTimeout(2500);
    if (!http.begin(client, cfg.ntfyUrl)) {
      Serial.println("[NTFY] HTTP begin() gagal.");
      return false;
    }

    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    http.addHeader("Title", "Irigasi Pot");
    http.addHeader("User-Agent", "IrigasiPot/1.0 ESP8266");

    httpCode = http.POST(message);
  }

  if (httpCode > 0) {
    Serial.printf("[NTFY] HTTP response: %d\n", httpCode);
    // Body response ntfy tidak diperlukan. Jangan panggil getString(),
    // karena itu dapat menahan loop ESP8266 lebih lama.

    bool ok = (httpCode >= 200 && httpCode < 300);
    if (!ok) {
      Serial.println("[NTFY] GAGAL: server ntfy menolak request.");
    } else {
      Serial.println("[NTFY] BERHASIL terkirim.");
    }

    http.end();
    return ok;
  }

  Serial.printf("[NTFY] Request gagal, error code: %d\n", httpCode);
  http.end();
  return false;
}

// ---------------------------------------------------------------------
// DEBUG / RELAY CONTROL
// ---------------------------------------------------------------------
const char* levelName(int level) {
  return level ? "HIGH" : "LOW";
}

void printRelayDebug(const char* event) {
  if (!DEBUG_RELAY) return;
  int actual = digitalRead(RELAY_PIN);
  Serial.printf(
    "[RELAY] %s | D7=%s | commanded=%s | pumpOn=%s | countON=%lu | countOFF=%lu | reason=%s | t=%lums\n",
    event,
    levelName(actual),
    levelName(relayLastCommandLevel),
    pumpOn ? "YES" : "NO",
    relayOnCount,
    relayOffCount,
    relayLastReason.c_str(),
    millis()
  );
}

void relayWrite(int level, const char* reason) {
  // ON  = OUTPUT + LOW (active-LOW)
  // OFF = INPUT_PULLUP / HIGH-Z + pull-up internal
  // Transisi dilakukan dengan latch aman terlebih dahulu.
  if (level == RELAY_ON) {
    digitalWrite(RELAY_PIN, LOW);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
  } else {
    digitalWrite(RELAY_PIN, HIGH);
    if (RELAY_OFF_USE_INPUT_PULLUP) {
      pinMode(RELAY_PIN, INPUT_PULLUP);
      digitalWrite(RELAY_PIN, HIGH);
    } else {
      pinMode(RELAY_PIN, OUTPUT);
      digitalWrite(RELAY_PIN, HIGH);
    }
  }

  relayLastCommandLevel = level;
  relayLastReason = reason ? String(reason) : String("-");
  if (level == RELAY_ON) relayOnCount++;
  else if (level == RELAY_OFF) relayOffCount++;
  printRelayDebug(level == RELAY_ON ? "CMD ON" : "CMD OFF");
}

void printSystemDebug(const char* where) {
  Serial.printf("[DEBUG] %s | uptime=%lus | heap=%u | reset=",
                where, millis() / 1000UL, ESP.getFreeHeap());
  Serial.println(ESP.getResetReason());
  Serial.printf("[DEBUG] relayPin=D7/GPIO%d | ON=%s | OFF=%s | OFF_MODE=%s | rawD7=%s | pumpOn=%s | manualOverride=%s\n",
                RELAY_PIN, levelName(RELAY_ON), levelName(RELAY_OFF),
                RELAY_OFF_USE_INPUT_PULLUP ? "INPUT_PULLUP" : "OUTPUT_HIGH",
                levelName(digitalRead(RELAY_PIN)), pumpOn ? "YES" : "NO",
                relayManualOverride ? "YES" : "NO");
  Serial.printf("[DEBUG] wifi=%s | timeSynced=%s | schedules=%u | lastWatered=%ld\n",
                WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "TERPUTUS",
                timeSynced ? "YES" : "NO",
                (unsigned)schedule_.size(),
                (long)devState.lastWateredEpoch);
}

void handleDebugSerial() {
  if (!DEBUG_SERIAL_COMMANDS || !Serial.available()) return;
  char c = (char)Serial.read();
  if (c == '0') {
    relayManualOverride = false;
    pumpOn = false;
    relayWrite(RELAY_OFF, "serial 0 / force OFF");
    Serial.println("[DEBUG] Relay dipaksa OFF (0), manual override dimatikan.");
  } else if (c == '1') {
    relayManualOverride = true;
    pumpOn = false;
    relayWrite(RELAY_ON, "serial 1 / force ON");
    Serial.println("[DEBUG] Relay dipaksa ON (1). Ketik 0 untuk OFF.");
  } else if (c == 't' || c == 'T') {
    relayManualOverride = !relayManualOverride;
    if (relayManualOverride) {
      pumpOn = false;
      relayWrite(RELAY_ON, "serial T / manual toggle ON");
      Serial.println("[DEBUG] Manual relay override = ON");
    } else {
      relayWrite(RELAY_OFF, "serial T / manual toggle OFF");
      Serial.println("[DEBUG] Manual relay override = OFF");
    }
  } else if (c == 's' || c == 'S') {
    printSystemDebug("Serial status request");
    printRelayDebug("STATUS");
  }
}

// ---------------------------------------------------------------------
// POMPA / PENYIRAMAN
// Non-blocking: relay dinyalakan lalu dimatikan lewat tickWatering() di
// loop(), supaya web server & pengecekan jadwal tidak ikut ke-block
// selama durasi siram (maks bisa sampai puluhan detik).
// ---------------------------------------------------------------------
bool startWatering(int durationSec, const String& reason) {
  if (pumpOn) return false;
  relayManualOverride = false;
  durationSec = constrain(durationSec, 1, cfg.maxDuration);
  relayWrite(RELAY_ON, reason.c_str());
  pumpOn = true;
  pumpStartMs = millis();
  pumpDurationMs = (unsigned long)durationSec * 1000UL;
  lastActivityMs = millis();
  Serial.printf("[POMPA] ON (%ds) - %s\n", durationSec, reason.c_str());
  return true;
}

void tickWatering() {
  if (pumpOn && millis() - pumpStartMs >= pumpDurationMs) {
    relayWrite(RELAY_OFF, "water cycle selesai");
    relayManualOverride = false;
    pumpOn = false;
    if (timeSynced) devState.lastWateredEpoch = nowEpoch();
    saveState();
    Serial.println("[POMPA] OFF");
    String notif = String("Penyiraman selesai (") + String(pumpDurationMs / 1000UL) + " detik)";
    bool ntfyOk = sendNtfy(notif);
    Serial.println(ntfyOk ? "[NTFY] Status akhir: OK" : "[NTFY] Status akhir: GAGAL");
  }
}

String scheduleRunKey(time_t slotEpoch) {
  struct tm ts;
  gmtime_r(&slotEpoch, &ts);
  char key[24];
  snprintf(key, sizeof(key), "%04d-%02d-%02d %02d:%02d",
           ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday,
           ts.tm_hour, ts.tm_min);
  return String(key);
}

// ---------------------------------------------------------------------
// SCHEDULER MINIMAL
// ---------------------------------------------------------------------
// Scheduler TIDAK mengendalikan GPIO/relay secara langsung.
// Saat waktunya tiba, scheduler hanya memanggil startWatering() yang sama
// dengan tombol "Siram sekarang" yang sudah terbukti bekerja.
// ---------------------------------------------------------------------
void checkSchedule() {
  if (!timeSynced) {
    if (DEBUG_SCHEDULE) Serial.println("[SCHED] Skip: waktu belum sinkron.");
    return;
  }

  if (pumpOn) {
    if (DEBUG_SCHEDULE) Serial.println("[SCHED] Skip: pompa sedang ON.");
    return;
  }

  // Saat sedang dipaksa manual lewat Serial, jangan ambil alih.
  if (relayManualOverride) {
    if (DEBUG_SCHEDULE) Serial.println("[SCHED] Skip: manual relay override aktif.");
    return;
  }

  if (schedule_.empty()) {
    if (DEBUG_SCHEDULE) Serial.println("[SCHED] Skip: tidak ada jadwal.");
    return;
  }

  time_t now = nowEpoch();

  if (DEBUG_SCHEDULE) {
    struct tm tn;
    gmtime_r(&now, &tn);
    Serial.printf("[SCHED] Cek %02d:%02d:%02d | slot=%u | lastWatered=%ld\n",
                  tn.tm_hour, tn.tm_min, tn.tm_sec,
                  (unsigned)schedule_.size(),
                  (long)devState.lastWateredEpoch);
  }

  for (auto &s : schedule_) {
    time_t slotEpoch = epochForTimeToday(now, s.time);
    long delta = (long)(now - slotEpoch);

    // Hanya boleh trigger dari waktu jadwal sampai maksimal
    // SCHEDULE_TOLERANCE_SEC setelah waktu jadwal.
    bool dueNow =
      (delta >= 0) &&
      ((unsigned long)delta <= SCHEDULE_TOLERANCE_SEC);

    String runKey = scheduleRunKey(slotEpoch);
    bool alreadyDone = devState.lastScheduleKey == runKey;

    if (DEBUG_SCHEDULE) {
      Serial.printf(
        "[SCHED] slot=%s dur=%ds delta=%ld due=%s done=%s key=%s\n",
        s.time.c_str(),
        s.duration,
        delta,
        dueNow ? "YES" : "NO",
        alreadyDone ? "YES" : "NO",
        runKey.c_str()
      );
    }

    if (dueNow && !alreadyDone) {
      // Kunci slot SEBELUM menyalakan pompa.
      // Jika ESP restart sesudah relay ON atau jika loop kembali memeriksa
      // slot yang sama, slot ini tetap dianggap sudah dipicu.
      devState.lastScheduleKey = runKey;
      saveState();

      // Tidak ada digitalWrite() relay di scheduler.
      // Gunakan jalur penyiraman yang sama dengan tombol manual.
      bool started = startWatering(s.duration, "jadwal " + s.time);

      if (started) {
        if (DEBUG_SCHEDULE) {
          Serial.printf("[SCHED] >>> JADWAL MEMICU startWatering() | key=%s <<<\n",
                        runKey.c_str());
        }
      } else {
        // Hanya batalkan marker jika benar-benar gagal start.
        devState.lastScheduleKey = "";
        saveState();
        if (DEBUG_SCHEDULE) {
          Serial.println("[SCHED] startWatering gagal, marker slot dibatalkan.");
        }
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------
// WIFI
// ---------------------------------------------------------------------
bool tryConnectSTA(const String& ssid, const String& password, unsigned long timeoutMs) {
  Serial.printf("[WIFI] Mencoba %s...\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.println(ok ? "[WIFI] Terhubung" : "[WIFI] Gagal");
  return ok;
}

void connectSavedNetworks() {
  for (auto &e : wifiList) {
    if (tryConnectSTA(e.ssid, e.password, WIFI_CONNECT_TIMEOUT_MS)) return;
  }
  if (!wifiList.empty()) Serial.println("[WIFI] Tidak ada jaringan tersimpan yang berhasil terhubung.");
}

// ---------------------------------------------------------------------
// WEB — HELPER
// ---------------------------------------------------------------------
void touchActivity() { lastActivityMs = millis(); }

void sendJson(int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendOk() {
  DynamicJsonDocument doc(64);
  doc["ok"] = true;
  sendJson(200, doc);
}

void sendError(int code, const String& msg) {
  DynamicJsonDocument doc(128);
  doc["error"] = msg;
  sendJson(code, doc);
}

bool serveFile(const char* path, const char* mime) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  server.streamFile(f, mime);
  f.close();
  return true;
}

// ---------------------------------------------------------------------
// WEB — ROUTES: HALAMAN
// ---------------------------------------------------------------------
void handleRoot() {
  if (!serveFile("/dashboard.html", "text/html"))
    server.send(404, "text/plain", "dashboard.html tidak ditemukan di LittleFS (jangan lupa upload folder data/)");
}
void handleWifiPage() {
  if (!serveFile("/wifi_manager.html", "text/html"))
    server.send(404, "text/plain", "wifi_manager.html tidak ditemukan di LittleFS (jangan lupa upload folder data/)");
}

// ---------------------------------------------------------------------
// WEB — ROUTES: STATUS (dipakai dashboard.html & wifi_manager.html)
// ---------------------------------------------------------------------
void handleGetStatus() {
  DynamicJsonDocument doc(768);
  bool connected = WiFi.status() == WL_CONNECTED;
  doc["wifiConnected"] = connected;
  doc["ssid"] = connected ? WiFi.SSID() : "";
  doc["lastWateredText"] = formatLastWatered(devState.lastWateredEpoch);
  doc["timeSynced"] = timeSynced;
  doc["lastScheduleKey"] = devState.lastScheduleKey;

  doc["pumpOn"] = pumpOn;
  if (pumpOn) {
    unsigned long elapsed = millis() - pumpStartMs;
    unsigned long total = pumpDurationMs;
    unsigned long remaining = (elapsed >= total) ? 0UL : (total - elapsed + 999UL) / 1000UL;
    doc["wateringRemainingSec"] = remaining;
  } else {
    doc["wateringRemainingSec"] = 0;
  }

  // Kirim sisa waktu relatif dari clock ESP, bukan epoch browser.
  time_t now = timeSynced ? nowEpoch() : 0;
  time_t next = timeSynced ? nextScheduleEpoch(now) : 0;
  if (next) {
    long remaining = (long)(next - now);
    if (remaining < 0) remaining = 0;
    doc["nextWaterInSec"] = remaining;

    struct tm ts;
    gmtime_r(&next, &ts);
    char label[6];
    snprintf(label, sizeof(label), "%02d:%02d", ts.tm_hour, ts.tm_min);
    doc["nextWaterLabel"] = label;
  } else {
    doc["nextWaterInSec"] = 0;
    doc["nextWaterLabel"] = "";
  }

  sendJson(200, doc);
}

// ---------------------------------------------------------------------
// WEB — ROUTES: SCHEDULE (dashboard.html)
// ---------------------------------------------------------------------
void handleGetSchedule() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("slots");
  for (auto &s : schedule_) {
    JsonObject o = arr.createNestedObject();
    o["time"] = s.time;
    o["duration"] = s.duration;
  }
  sendJson(200, doc);
}

void handlePostSchedule() {
  DynamicJsonDocument in(2048);
  if (deserializeJson(in, server.arg("plain"))) { sendError(400, "JSON tidak valid"); return; }
  if (!in.containsKey("slots") || !in["slots"].is<JsonArray>()) {
    sendError(400, "field 'slots' wajib berupa array"); return;
  }

  std::vector<ScheduleSlot> newSlots;
  for (JsonObject o : in["slots"].as<JsonArray>()) {
    String t = String((const char*)(o["time"] | ""));
    int d = o["duration"] | 0;
    if (t.length() != 5 || t.charAt(2) != ':') { sendError(400, "format waktu salah: " + t); return; }
    if (newSlots.size() >= 12) { sendError(400, "maksimal 12 jadwal"); return; }
    d = constrain(d, 1, cfg.maxDuration);
    newSlots.push_back({t, d});
  }
  schedule_ = newSlots;
  saveSchedule();
  touchActivity();
  sendOk();
}

// ---------------------------------------------------------------------
// WEB — ROUTES: WATER NOW (dashboard.html)
// ---------------------------------------------------------------------
void handleWaterNow() {
  if (pumpOn) { sendError(409, "Pompa sedang menyala"); return; }
  int dur = constrain(cfg.manualWaterDuration, 1, cfg.maxDuration);
  if (!startWatering(dur, "manual dari dashboard")) {
    sendError(409, "Pompa sedang menyala");
    return;
  }
  touchActivity();

  DynamicJsonDocument doc(128);
  doc["ok"] = true;
  doc["durationSec"] = dur;
  doc["pumpOn"] = pumpOn;
  doc["wateringRemainingSec"] = dur;
  sendJson(200, doc);
}

// ---------------------------------------------------------------------
// WEB — ROUTES: CONFIG (dashboard.html, panel Pengaturan)
// ---------------------------------------------------------------------
void handleGetConfig() {
  DynamicJsonDocument doc(384);
  doc["ntfyUrl"] = cfg.ntfyUrl;
  doc["maxDuration"] = cfg.maxDuration;
  doc["manualWaterDuration"] = cfg.manualWaterDuration;
  sendJson(200, doc);
}

void handlePostConfig() {
  DynamicJsonDocument in(384);
  if (deserializeJson(in, server.arg("plain"))) { sendError(400, "JSON tidak valid"); return; }
  String url = String((const char*)(in["ntfyUrl"] | cfg.ntfyUrl.c_str()));
  int maxDur = in["maxDuration"] | cfg.maxDuration;
  int manualDur = in["manualWaterDuration"] | cfg.manualWaterDuration;
  url = normalizeNtfyUrl(url);
  maxDur = constrain(maxDur, 1, 600);
  manualDur = constrain(manualDur, 1, maxDur);
  cfg.ntfyUrl = url;
  cfg.maxDuration = maxDur;
  cfg.manualWaterDuration = manualDur;
  saveConfig();
  touchActivity();

  DynamicJsonDocument doc(128);
  doc["ok"] = true;
  doc["maxDuration"] = cfg.maxDuration;
  doc["manualWaterDuration"] = cfg.manualWaterDuration;
  sendJson(200, doc);
}

// ---------------------------------------------------------------------
// WEB — ROUTES: WIFI (wifi_manager.html)
// ---------------------------------------------------------------------
void handleGetWifiList() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  String curSsid = WiFi.SSID();
  bool connected = WiFi.status() == WL_CONNECTED;
  for (auto &e : wifiList) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = e.ssid;
    o["password"] = e.password; // wifi_manager.html memang menampilkan password di panel detail
    o["connected"] = connected && curSsid == e.ssid;
  }
  sendJson(200, doc);
}

void handlePostWifi() {
  DynamicJsonDocument in(512);
  if (deserializeJson(in, server.arg("plain"))) { sendError(400, "JSON tidak valid"); return; }
  String ssid = String((const char*)(in["ssid"] | ""));
  String pass = String((const char*)(in["password"] | ""));
  if (!ssid.length()) { sendError(400, "ssid wajib diisi"); return; }
  int idx = findWifiIndex(ssid);
  if (idx >= 0) wifiList[idx].password = pass;
  else wifiList.push_back({ssid, pass});
  saveWifiList();
  touchActivity();
  sendOk();
}

void handleDeleteWifi() {
  String ssid = server.arg("ssid");
  if (!ssid.length()) { sendError(400, "parameter ssid wajib diisi"); return; }
  int idx = findWifiIndex(ssid);
  if (idx < 0) { sendError(404, "ssid tidak ditemukan"); return; }
  wifiList.erase(wifiList.begin() + idx);
  saveWifiList();
  touchActivity();
  sendOk();
}

// Scan async: panggilan pertama memicu WiFi.scanNetworks(true,...) dan
// langsung balas {"scanning":true}; frontend polling GET ini berulang
// sampai hasil siap (dipakai oleh wifi_manager.html tombol "Pindai").
void sendScanning() {
  DynamicJsonDocument doc(64);
  doc["scanning"] = true;
  sendJson(200, doc);
}

void handleWifiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { sendScanning(); return; }
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true /*async*/, false /*jangan tampilkan hidden SSID*/);
    sendScanning();
    return;
  }
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("networks");
  std::vector<String> seen;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue; // lewati hidden SSID
    bool dup = false;
    for (auto &s : seen) if (s == ssid) { dup = true; break; }
    if (dup) continue; // beberapa AP/mesh bisa muncul >1x dgn SSID sama
    seen.push_back(ssid);
    JsonObject o = arr.createNestedObject();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
  }
  WiFi.scanDelete();
  touchActivity();
  sendJson(200, doc);
}

void handleWifiConnect() {
  DynamicJsonDocument in(512);
  if (deserializeJson(in, server.arg("plain"))) { sendError(400, "JSON tidak valid"); return; }
  String ssid = String((const char*)(in["ssid"] | ""));
  String pass = String((const char*)(in["password"] | ""));
  if (!ssid.length()) { sendError(400, "ssid wajib diisi"); return; }
  touchActivity();
  // Non-blocking: mulai proses koneksi STA, hasil akhirnya dicek lewat GET /api/status
  WiFi.begin(ssid.c_str(), pass.c_str());
  sendOk();
}

void handleRelayControl() {
  String state;
  int durationSec = 0;

  if (server.hasArg("plain") && server.arg("plain").length()) {
    DynamicJsonDocument in(256);
    if (deserializeJson(in, server.arg("plain"))) {
      sendError(400, "JSON tidak valid");
      return;
    }
    state = String((const char*)(in["state"] | ""));
    durationSec = in["duration"] | 0;
  } else {
    state = server.arg("state");
    durationSec = server.arg("duration").toInt();
  }

  state.toLowerCase();
  touchActivity();

  if (state == "on") {
    relayManualOverride = true;
    pumpOn = false;
    relayWrite(RELAY_ON, "HTTP /api/relay state=on");
  } else if (state == "off") {
    relayManualOverride = false;
    pumpOn = false;
    relayWrite(RELAY_OFF, "HTTP /api/relay state=off");
  } else if (state == "water") {
    relayManualOverride = false;
    if (durationSec <= 0) durationSec = MANUAL_WATER_DEFAULT_SEC;
    if (!startWatering(durationSec, "HTTP /api/relay state=water")) {
      sendError(409, "Pompa sedang menyiram");
      return;
    }
  } else {
    sendError(400, "state harus on, off, atau water");
    return;
  }

  DynamicJsonDocument out(256);
  out["ok"] = true;
  out["state"] = (relayManualOverride || pumpOn) ? "on" : "off";
  out["rawD7"] = digitalRead(RELAY_PIN);
  out["commanded"] = relayLastCommandLevel;
  out["manualOverride"] = relayManualOverride;
  out["pumpOn"] = pumpOn;
  sendJson(200, out);
}

void handleNtfyTest() {
  if (cfg.ntfyUrl.length() == 0) {
    sendError(400, "URL ntfy belum diisi");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    sendError(503, "ESP8266 belum terhubung ke Wi-Fi internet");
    return;
  }

  bool ok = sendNtfy("Tes notifikasi dari Irigasi Pot");
  if (!ok) {
    sendError(502, "Pengiriman ntfy gagal. Lihat Serial Monitor untuk detail.");
    return;
  }

  sendOk();
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ---------------------------------------------------------------------
// DEEP SLEEP
// Dipanggil setelah idle timeout & tidak sedang menyiram. Menghitung
// selisih waktu ke jadwal berikutnya, dipecah per siklus max
// SLEEP_CHUNK_SEC (lihat plan_infor.md bagian 5 soal batas ~71 menit
// ESP.deepSleep()). ESP.deepSleep() = reset penuh; setup() akan
// berjalan ulang dari awal saat device bangun lagi.
// ---------------------------------------------------------------------
void goToSleep() {
  Serial.println("[SLEEP] Idle timeout, mempersiapkan deep sleep...");
  server.stop();

  unsigned long sleepSec;
  if (!timeSynced) {
    sleepSec = NO_TIME_FALLBACK_SEC;
  } else {
    time_t now = nowEpoch();
    time_t next = nextScheduleEpoch(now);
    long diff = next ? (long)(next - now) : (long)SLEEP_CHUNK_SEC;
    if (diff < 10) diff = 10;
    sleepSec = (unsigned long)min((long)SLEEP_CHUNK_SEC, diff);
  }

  Serial.printf("[SLEEP] Tidur %lu detik\n", sleepSec);
  Serial.flush();
  WiFi.mode(WIFI_OFF);
  delay(50);
  ESP.deepSleep((uint64_t)sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
  delay(200); // hanya sampai sini kalau D0 belum tersambung ke RST
}

// ---------------------------------------------------------------------
// SETUP & LOOP
// ---------------------------------------------------------------------
void setup() {
  // Inisialisasi relay SEBELUM WiFi/FS agar kondisi aman ditetapkan paling awal.
  int rawBeforePinMode = digitalRead(RELAY_PIN);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(RELAY_PIN, INPUT_PULLUP);    // OFF = HIGH-Z + pull-up
  digitalWrite(RELAY_PIN, HIGH);
  relayLastCommandLevel = RELAY_OFF;
  relayLastReason = "startup safety";
  relayOffCount++;
  pumpOn = false;
  relayManualOverride = false;
  bootMs = millis();
  delay(RELAY_BOOT_SETTLE_MS);

  Serial.begin(115200);
  delay(100);
  Serial.println("\n[BOOT] IrigasiPot starting...");
  Serial.printf("[BOOT] Reset reason: %s\n", ESP.getResetReason().c_str());
  Serial.printf("[RELAY] Sebelum pinMode: D7=%s\n", levelName(rawBeforePinMode));
  Serial.printf("[RELAY] Setelah init:  D7=%s | ON=%s | OFF=%s\n",
                levelName(digitalRead(RELAY_PIN)), levelName(RELAY_ON), levelName(RELAY_OFF));
  printRelayDebug("BOOT SAFETY OFF");
  Serial.printf("[RELAY] OFF mode=%s | ON mode=OUTPUT-LOW\n",
                RELAY_OFF_USE_INPUT_PULLUP ? "INPUT_PULLUP/HIGH-Z" : "OUTPUT-HIGH");

  if (!LittleFS.begin()) {
    Serial.println("[FS] LittleFS gagal mount, mencoba format...");
    LittleFS.format();
    LittleFS.begin();
  }

  loadWifiList();
  loadSchedule();
  loadConfig();
  loadState();

  Serial.printf("[FS] WiFi tersimpan : %u\n", (unsigned)wifiList.size());
  Serial.printf("[FS] Jadwal tersimpan: %u\n", (unsigned)schedule_.size());
  Serial.printf("[FS] maxDuration     : %ds\n", cfg.maxDuration);
  Serial.printf("[FS] manualWater     : %ds\n", cfg.manualWaterDuration);
  Serial.printf("[FS] ntfyUrl         : %s\n", cfg.ntfyUrl.length() ? cfg.ntfyUrl.c_str() : "(kosong)");
  for (auto &s : schedule_) {
    Serial.printf("[FS]   jadwal %s = %ds\n", s.time.c_str(), s.duration);
  }
  Serial.printf("[STATE] lastWateredEpoch=%ld\n", (long)devState.lastWateredEpoch);

  // AP+STA selalu aktif — dashboard tetap bisa diakses lewat hotspot sendiri
  // walau WiFi rumah/sekolah belum/tidak tersambung.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) ? AP_PASSWORD : NULL);
  Serial.print("[AP] SSID: "); Serial.println(AP_SSID);
  Serial.print("[AP] IP  : "); Serial.println(WiFi.softAPIP());

  connectSavedNetworks();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] IP STA: "); Serial.println(WiFi.localIP());
    timeSynced = syncTime();
    Serial.println(timeSynced ? "[NTP] Waktu tersinkron" : "[NTP] Gagal sinkron waktu");
    if (timeSynced) {
      time_t now = nowEpoch();
      struct tm tn;
      gmtime_r(&now, &tn);
      Serial.printf("[NTP] Waktu lokal firmware: %04d-%02d-%02d %02d:%02d:%02d\n",
                    tn.tm_year + 1900, tn.tm_mon + 1, tn.tm_mday,
                    tn.tm_hour, tn.tm_min, tn.tm_sec);
      time_t next = nextScheduleEpoch(now);
      if (next) {
        struct tm ts;
        gmtime_r(&next, &ts);
        Serial.printf("[SCHED] Jadwal berikutnya: %04d-%02d-%02d %02d:%02d:%02d\n",
                      ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday,
                      ts.tm_hour, ts.tm_min, ts.tm_sec);
      } else {
        Serial.println("[SCHED] Tidak ada jadwal berikutnya.");
      }
    }
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/schedule", HTTP_GET, handleGetSchedule);
  server.on("/api/schedule", HTTP_POST, handlePostSchedule);
  server.on("/api/water-now", HTTP_POST, handleWaterNow);
  server.on("/api/relay", HTTP_POST, handleRelayControl);
  server.on("/api/relay", HTTP_GET, handleRelayControl);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/ntfy-test", HTTP_POST, handleNtfyTest);
  server.on("/api/wifi", HTTP_GET, handleGetWifiList);
  server.on("/api/wifi", HTTP_POST, handlePostWifi);
  server.on("/api/wifi", HTTP_DELETE, handleDeleteWifi);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/debug", HTTP_GET, []() {
    DynamicJsonDocument doc(768);
    doc["relayPin"] = RELAY_PIN;
    doc["relayOnLevel"] = RELAY_ON;
    doc["relayOffLevel"] = RELAY_OFF;
    doc["relayRaw"] = digitalRead(RELAY_PIN);
    doc["relayCommanded"] = relayLastCommandLevel;
    doc["relayCommandedName"] = levelName(relayLastCommandLevel);
    doc["relayLastReason"] = relayLastReason;
    doc["relayOffMode"] = RELAY_OFF_USE_INPUT_PULLUP ? "INPUT_PULLUP/HIGH-Z" : "OUTPUT_HIGH";
    doc["manualOverride"] = relayManualOverride;
    doc["pumpOn"] = pumpOn;
    doc["relayOnCount"] = relayOnCount;
    doc["relayOffCount"] = relayOffCount;
    doc["uptimeMs"] = millis();
    doc["startupGuardMs"] = STARTUP_SCHEDULER_GUARD_MS;
    doc["timeSynced"] = timeSynced;
    doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    doc["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
    doc["scheduleCount"] = (unsigned)schedule_.size();
    doc["lastWateredEpoch"] = (long)devState.lastWateredEpoch;
    doc["lastScheduleKey"] = devState.lastScheduleKey;
    doc["ntfyConfigured"] = cfg.ntfyUrl.length() > 0;
    doc["ntfyUrl"] = cfg.ntfyUrl;
    doc["ntfyWifiConnected"] = WiFi.status() == WL_CONNECTED;
    sendJson(200, doc);
  });
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] Server dimulai");

  lastActivityMs = millis();
  lastScheduleCheckMs = millis();

  printSystemDebug("SETUP SELESAI");
  lastRelayMonitorMs = millis();
  Serial.printf("[SCHED] Scheduler guard berakhir dalam %lums\n",
                STARTUP_SCHEDULER_GUARD_MS);

  // Scheduler dijalankan berkala dari loop().
}

void loop() {
  handleDebugSerial();
  server.handleClient();
  tickWatering();

  // Fail-safe software: relay OFF harus terus dijaga saat tidak menyiram,
  // kecuali sedang dalam manual relay override untuk pengujian.
  if (!pumpOn && !relayManualOverride && digitalRead(RELAY_PIN) != RELAY_OFF) {
    Serial.printf("[RELAY][WARN] Pump OFF tapi D7=%s, expected=%s -> paksa OFF\n",
                  levelName(digitalRead(RELAY_PIN)), levelName(RELAY_OFF));
    relayWrite(RELAY_OFF, "loop fail-safe OFF");
  }

  if (millis() - lastRelayMonitorMs >= 2000UL) {
    lastRelayMonitorMs = millis();
    int actual = digitalRead(RELAY_PIN);
    if (actual != relayLastCommandLevel) {
      Serial.printf("[RELAY][MISMATCH] commanded=%s actual=%s pumpOn=%s\n",
                    levelName(relayLastCommandLevel), levelName(actual), pumpOn ? "YES" : "NO");
    } else {
      Serial.printf("[RELAY][MON] D7=%s commanded=%s pumpOn=%s\n",
                    levelName(actual), levelName(relayLastCommandLevel), pumpOn ? "YES" : "NO");
    }
  }

  // Keep-alive ON: pastikan GPIO tetap mode OUTPUT/LOW selama penyiraman.
  // Ini membantu jika ada kode/periferal lain yang sempat mengubah mode pin.
  if ((pumpOn || relayManualOverride) && digitalRead(RELAY_PIN) != RELAY_ON) {
    relayWrite(RELAY_ON, pumpOn ? "pump keep-alive" : "manual keep-alive");
  }

  maintainTimeSync();

  if (millis() - lastScheduleCheckMs > 5000UL) {
    if (timeSynced) timeClient.update();
    checkSchedule();
    lastScheduleCheckMs = millis();
  }

  if (!pumpOn && !relayManualOverride && millis() - lastActivityMs > IDLE_TIMEOUT_MS) {
    goToSleep();
  }
}
