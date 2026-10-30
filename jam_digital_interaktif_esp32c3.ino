/*
  PROJECT : JAM DIGITAL INTERAKTIF BERBASIS WEB
  MCU     : ESP32-C3 Super Mini
  DISPLAY : MAX7219, 4 modul matrix 8x8 (32x8)
  TIME    : RTC DS1307 dengan sinkronisasi waktu dari web browser.

  Fitur sesuai dokumen UTS:
  - Jam real-time dan detik pada halaman web.
  - Hari, tanggal, bulan, dan tahun.
  - Format 12 jam (AM/PM) atau 24 jam.
  - Pengaturan tanggal/waktu manual melalui GUI web.
  - Sinkronisasi otomatis dari waktu lokal HP/laptop ketika halaman dibuka.
  - Mode LED: jam, tanggal, atau otomatis.
  - Pengaturan kecerahan dan durasi mode tersimpan di Preferences.
  - Web server dan animasi display berjalan tanpa delay blocking di loop().
  - Hotspot setup selalu aktif; koneksi router dicoba di latar belakang.

  Library Arduino IDE:
  - MD_Parola
  - MD_MAX72XX
  - RTClib by Adafruit

  Wiring default (ubah melalui bagian Config jika rangkaian berbeda):
  MAX7219: DIN GPIO6, CLK GPIO4, CS GPIO7, VCC 5V, GND GND
  DS1307 : SDA GPIO0, SCL GPIO1, VCC 5V, GND GND
  Tombol opsional: GPIO3 ke tombol lalu GND

  Catatan:
  DS1307 memerlukan baterai yang baik agar waktu tetap tersimpan saat daya utama mati.
  Jika RTC tidak terdeteksi, sinkronisasi browser masih berfungsi selama ESP32 menyala,
  tetapi waktu tidak dapat bertahan setelah daya ESP32 mati. Waktu NTP digunakan jika
  ESP32 berhasil terhubung ke router dengan internet.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <RTClib.h>
#include <time.h>
#include <stdlib.h>

// ============================ KONFIGURASI ============================
namespace Config {
  // Wi-Fi rumah/kampus. Biarkan sebagai nilai default untuk memakai hotspot saja.
  constexpr char WIFI_SSID[] = "LENOVO 3721";
  constexpr char WIFI_PASSWORD[] = "12345678";

  constexpr char AP_SSID[] = "JamDigital";
  constexpr char AP_PASSWORD[] = "12345678";
  constexpr char MDNS_HOSTNAME[] = "jamdigital";
  constexpr char TIMEZONE_POSIX[] = "WIB-7";
  constexpr char NTP_SERVER_1[] = "pool.ntp.org";
  constexpr char NTP_SERVER_2[] = "time.google.com";

  constexpr uint8_t PIN_CLK = 4;
  constexpr uint8_t PIN_DIN = 6;
  constexpr uint8_t PIN_CS = 7;
  constexpr uint8_t PIN_BUTTON = 3;
  constexpr uint8_t PIN_SDA = 0;
  constexpr uint8_t PIN_SCL = 1;

  constexpr auto DISPLAY_HARDWARE = MD_MAX72XX::FC16_HW;
  constexpr uint8_t DISPLAY_MODULES = 4;
  constexpr uint8_t BRIGHTNESS_DEFAULT = 4;
  constexpr uint8_t BRIGHTNESS_MIN = 0;
  constexpr uint8_t BRIGHTNESS_MAX = 15;
  constexpr uint16_t DISPLAY_DURATION_DEFAULT_SEC = 15;
  constexpr uint16_t DISPLAY_DURATION_MIN_SEC = 5;
  constexpr uint16_t DISPLAY_DURATION_MAX_SEC = 60;
  constexpr uint16_t DISPLAY_SCROLL_SPEED_MS = 45;
  constexpr uint16_t CLOCK_REFRESH_MS = 500;
  constexpr uint16_t BUTTON_DEBOUNCE_MS = 40;
  constexpr uint32_t WIFI_RETRY_MS = 15000UL;
  constexpr uint32_t RTC_RETRY_MS = 5000UL;
  constexpr uint32_t NTP_CHECK_MS = 1000UL;
  constexpr uint32_t HTTP_TIMEOUT_MS = 8000UL;
  constexpr uint32_t NTP_MIN_VALID_EPOCH = 1577836800UL; // 2020-01-01 UTC
  constexpr uint16_t YEAR_MIN = 2020;
  constexpr uint16_t YEAR_MAX = 2099;
  constexpr uint16_t WEB_PORT = 80;
  constexpr uint32_t SERIAL_BAUD = 115200UL;
  constexpr uint16_t HTTP_OK = 200;
  constexpr uint16_t HTTP_BAD_REQUEST = 400;
  constexpr uint16_t HTTP_NOT_FOUND = 404;
  constexpr uint16_t HTTP_SERVICE_UNAVAILABLE = 503;
}

// ============================ OBJEK GLOBAL ============================
MD_Parola display(Config::DISPLAY_HARDWARE, Config::PIN_CS, Config::DISPLAY_MODULES);
RTC_DS1307 rtc;
WebServer server(Config::WEB_PORT);
Preferences preferences;

// ============================ DATA WAKTU ============================
const char* const NAMA_HARI[] = {
  "Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"
};
const char* const NAMA_BULAN[] = {
  "Januari", "Februari", "Maret", "April", "Mei", "Juni",
  "Juli", "Agustus", "September", "Oktober", "November", "Desember"
};

enum class DisplayPreference : uint8_t { CLOCK = 0, DATE = 1, AUTO = 2 };
enum class DisplayPage : uint8_t { CLOCK = 0, DATE = 1 };
enum class TimeFormat : uint8_t { HOUR_24 = 24, HOUR_12 = 12 };

bool rtcAvailable = false;
bool timeConfigured = false;
bool userSetTimeThisBoot = false;
bool ntpConfigured = false;
bool ntpSynchronized = false;
bool mdnsStarted = false;
bool wifiAttemptStarted = false;
bool lastWifiConnected = false;
bool buttonPrevious = HIGH;

DateTime fallbackBase(2026, 1, 1, 0, 0, 0);
uint32_t fallbackBaseMillis = 0;
DisplayPreference displayPreference = DisplayPreference::AUTO;
DisplayPage activeDisplayPage = DisplayPage::CLOCK;
TimeFormat timeFormat = TimeFormat::HOUR_24;
uint8_t brightness = Config::BRIGHTNESS_DEFAULT;
uint16_t displayDurationSeconds = Config::DISPLAY_DURATION_DEFAULT_SEC;
uint32_t pageStartedMillis = 0;
uint32_t lastClockRefreshMillis = 0;
uint32_t lastWifiAttemptMillis = 0;
uint32_t lastRtcProbeMillis = 0;
uint32_t lastNtpCheckMillis = 0;
uint32_t buttonPressedMillis = 0;
char clockBuffer[16] = {0};
char dateBuffer[96] = {0};
const char* timeSource = "Waktu kompilasi (belum disinkronkan)";

const char* hariNama(uint8_t index) {
  return index < (sizeof(NAMA_HARI) / sizeof(NAMA_HARI[0])) ? NAMA_HARI[index] : "?";
}

const char* bulanNama(uint8_t month) {
  return month >= 1 && month <= (sizeof(NAMA_BULAN) / sizeof(NAMA_BULAN[0]))
    ? NAMA_BULAN[month - 1] : "?";
}

bool tahunValid(uint16_t year) {
  return year >= Config::YEAR_MIN && year <= Config::YEAR_MAX;
}

bool tanggalWaktuValid(int year, int month, int day, int hour, int minute, int second) {
  if (year < Config::YEAR_MIN || year > Config::YEAR_MAX ||
      month < 1 || month > 12 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59) {
    return false;
  }
  const uint8_t daysByMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t maxDay = daysByMonth[month - 1];
  const bool leapYear = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
  if (month == 2 && leapYear) maxDay = 29;
  return day >= 1 && day <= maxDay;
}

void setFallbackTime(const DateTime& value) {
  fallbackBase = value;
  fallbackBaseMillis = millis();
}

DateTime fallbackTimeNow() {
  const uint32_t elapsedSeconds = (millis() - fallbackBaseMillis) / 1000UL;
  return fallbackBase + TimeSpan(static_cast<int32_t>(elapsedSeconds));
}

DateTime currentDateTime() {
  if (rtcAvailable) {
    const DateTime rtcTime = rtc.now();
    if (tahunValid(rtcTime.year())) {
      setFallbackTime(rtcTime);
      return rtcTime;
    }
  }
  return fallbackTimeNow();
}

void setDateTimeFromTimeStruct(const tm& localTime, const char* sourceName) {
  const DateTime value(
    localTime.tm_year + 1900,
    localTime.tm_mon + 1,
    localTime.tm_mday,
    localTime.tm_hour,
    localTime.tm_min,
    localTime.tm_sec
  );
  setFallbackTime(value);
  if (rtcAvailable) rtc.adjust(value);
  timeConfigured = true;
  timeSource = sourceName;
}

bool saveDateTime(int year, int month, int day, int hour, int minute, int second,
                  const char* sourceName, String& resultMessage) {
  if (!tanggalWaktuValid(year, month, day, hour, minute, second)) {
    resultMessage = "Tanggal atau waktu tidak valid.";
    return false;
  }

  const DateTime newTime(year, month, day, hour, minute, second);
  setFallbackTime(newTime);
  timeConfigured = true;
  userSetTimeThisBoot = true;
  timeSource = sourceName;

  if (!rtcAvailable) {
    resultMessage = "Waktu disetel sementara. RTC DS1307 tidak terdeteksi; waktu akan hilang saat daya ESP32 mati.";
    return true;
  }

  rtc.adjust(newTime);
  const DateTime readBack = rtc.now();
  const int32_t difference = static_cast<int32_t>(readBack.unixtime()) -
                             static_cast<int32_t>(newTime.unixtime());
  if (!tahunValid(readBack.year()) || difference < -1 || difference > 1) {
    resultMessage = "Penulisan RTC gagal diverifikasi. Periksa kabel SDA/SCL, daya, dan modul DS1307.";
    return false;
  }

  resultMessage = "Waktu berhasil disimpan dan diverifikasi pada RTC DS1307.";
  return true;
}

// ============================ HALAMAN WEB ============================
const char WEB_PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#101827">
<title>Jam Digital Interaktif</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--panel:#121e31;--panel2:#18273d;--line:#2a3b55;--text:#edf4ff;--muted:#9eb0c8;--accent:#6ee7d2;--blue:#60a5fa;--good:#86efac;--bad:#fda4af;--radius:20px}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(ellipse at 50% -10%,#203757 0,transparent 55%),var(--bg);color:var(--text);font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:18px 12px 32px}main{max-width:780px;margin:0 auto}.header{display:flex;justify-content:space-between;align-items:center;gap:12px;margin:4px 0 18px}.brand{display:flex;align-items:center;gap:12px;min-width:0}.logo{flex:0 0 46px;height:46px;border-radius:15px;display:grid;place-items:center;color:#06272e;background:linear-gradient(135deg,var(--accent),var(--blue));font-weight:900;font-size:17px}.eyebrow{font-size:10px;text-transform:uppercase;letter-spacing:.16em;color:var(--accent);font-weight:800}.title{font-size:clamp(19px,4vw,23px);margin:3px 0 0;letter-spacing:-.035em}.connect{display:flex;align-items:center;gap:7px;border:1px solid var(--line);background:#111d2e;padding:8px 10px;border-radius:999px;color:var(--muted);font-size:11px;white-space:nowrap}.dot{width:8px;height:8px;border-radius:50%;background:#fbbf24}.card{border:1px solid var(--line);border-radius:var(--radius);background:linear-gradient(145deg,rgba(24,39,61,.96),rgba(16,28,45,.96));padding:20px;margin:12px 0;box-shadow:0 18px 45px #0002}.clock-card{text-align:center;padding:29px 14px 23px;overflow:hidden}.section-label{font-size:10px;letter-spacing:.17em;text-transform:uppercase;color:var(--muted);font-weight:800}.clock{font-size:clamp(48px,12vw,86px);font-weight:760;line-height:1.15;letter-spacing:-.065em;font-variant-numeric:tabular-nums;margin:10px 0 7px;white-space:nowrap}.period{font-size:13px;color:var(--accent);font-weight:800;min-height:18px;letter-spacing:.1em}.date{font-size:clamp(15px,3.5vw,19px);color:#dbeafe;margin-top:7px}.source{font-size:11px;color:var(--muted);margin-top:12px;line-height:1.6;overflow-wrap:anywhere}.section-top{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:0 0 15px}.section-title{font-size:15px;margin:0;font-weight:750}.tag{font-size:10px;color:var(--muted);border:1px solid var(--line);background:#0e1929;border-radius:8px;padding:6px 8px;white-space:nowrap}.hint{font-size:12px;color:var(--muted);line-height:1.6;margin:0}.segmented{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:7px}.segmented.two{grid-template-columns:repeat(2,minmax(0,1fr))}.btn{font:inherit;font-size:13px;font-weight:700;border:1px solid var(--line);color:var(--text);background:#0e1929;padding:12px 9px;border-radius:12px;cursor:pointer;transition:background .15s,border-color .15s;min-width:0}.btn:hover{border-color:#56718e}.btn.active{background:#123b40;border-color:var(--accent);color:var(--accent)}.btn.primary{background:linear-gradient(110deg,var(--accent),#7dd3fc);color:#08222b;border:0}.btn:disabled{opacity:.55;cursor:wait}.field{margin-top:18px}.field-head{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13px;margin-bottom:8px}.value{font-variant-numeric:tabular-nums;color:var(--accent);font-weight:800}input[type=range]{width:100%;accent-color:var(--accent)}input[type=datetime-local],select{width:100%;min-width:0;color:var(--text);font:inherit;background:#0e1929;border:1px solid var(--line);border-radius:12px;padding:13px;margin-top:10px;color-scheme:dark}label{display:block;font-size:13px;font-weight:650;margin-top:16px}.actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:9px}.save-row{display:grid;grid-template-columns:1fr;gap:8px;margin-top:10px}.message{min-height:22px;text-align:center;font-size:12px;line-height:1.5;color:var(--good);margin:12px 2px 0}.message.error{color:var(--bad)}.footer{text-align:center;color:#7f91aa;font-size:10px;line-height:1.8;margin:18px 0 0}.connection-note{border-left:3px solid var(--blue);padding:9px 11px;background:#0e1929;border-radius:0 9px 9px 0;margin-top:12px;font-size:11px;color:var(--muted);line-height:1.6;overflow-wrap:anywhere}.topline{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:12px}.mini{border:1px solid var(--line);background:#0e1929;border-radius:13px;padding:11px}.mini-label{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}.mini-value{font-size:12px;font-weight:750;margin-top:5px;overflow-wrap:anywhere}
@media(max-width:440px){body{padding:12px 9px 25px}.card{padding:16px;border-radius:17px}.header{align-items:flex-start}.connect{font-size:10px;padding:7px 8px}.logo{flex-basis:40px;height:40px;border-radius:13px}.clock-card{padding:25px 8px 20px}.btn{padding:11px 5px;font-size:12px}.segmented{gap:5px}.topline{gap:7px}}
</style>
</head>
<body><main>
<header class="header"><div class="brand"><div class="logo">JD</div><div><div class="eyebrow">ESP32-C3 · WEB CONTROL</div><h1 class="title">Jam Digital Interaktif</h1></div></div><div class="connect"><span class="dot" id="connectionDot"></span><span id="connectionText">Menghubungkan</span></div></header>
<section class="card clock-card"><div class="section-label">Waktu pada perangkat</div><div class="clock" id="clockText">--:--:--</div><div class="period" id="periodText"></div><div class="date" id="dateText">Memuat hari dan tanggal...</div><div class="source" id="sourceText">Menghubungkan ke ESP32-C3...</div><div class="topline"><div class="mini"><div class="mini-label">Sumber waktu</div><div class="mini-value" id="timeSource">Memeriksa...</div></div><div class="mini"><div class="mini-label">RTC DS1307</div><div class="mini-value" id="rtcStatus">Memeriksa...</div></div></div></section>
<section class="card"><div class="section-top"><h2 class="section-title">Format waktu</h2><span class="tag">12 jam / 24 jam</span></div><p class="hint">Pilih format jam yang ditampilkan pada website dan display LED.</p><div class="segmented two" style="margin-top:12px"><button class="btn" id="format24" onclick="setFormat(24)">24 jam · 13:45</button><button class="btn" id="format12" onclick="setFormat(12)">12 jam · 01:45 PM</button></div></section>
<section class="card"><div class="section-top"><h2 class="section-title">Kontrol display LED</h2><span class="tag">MAX7219 · 32×8</span></div><div class="segmented"><button class="btn" id="modeClock" onclick="setDisplayMode('jam')">Jam</button><button class="btn" id="modeDate" onclick="setDisplayMode('tanggal')">Tanggal</button><button class="btn" id="modeAuto" onclick="setDisplayMode('auto')">Otomatis</button></div><div class="field"><div class="field-head"><label for="brightness" style="margin:0">Kecerahan LED</label><span class="value"><span id="brightnessValue">4</span>/15</span></div><input id="brightness" type="range" min="0" max="15" value="4" oninput="text('brightnessValue',this.value)" onchange="setNumber('/api/brightness',this.value)"></div><div class="field"><div class="field-head"><label for="duration" style="margin:0">Durasi tampilan jam</label><span class="value"><span id="durationValue">15</span> detik</span></div><input id="duration" type="range" min="5" max="60" value="15" oninput="text('durationValue',this.value)" onchange="setNumber('/api/duration',this.value)"><p class="hint">Digunakan ketika mode Otomatis dipilih.</p></div></section>
<section class="card"><div class="section-top"><h2 class="section-title">Pengaturan waktu</h2><span class="tag" id="syncBadge">Belum disinkronkan</span></div><p class="hint">Saat halaman dibuka, waktu lokal HP/laptop dikirim otomatis ke ESP32. Kamu juga bisa menyetel waktu secara manual.</p><div class="save-row"><button class="btn primary" id="syncButton" onclick="syncFromPhone()">⟳ Sinkronkan dari HP / laptop</button></div><label for="manualTime">Atur tanggal dan jam secara manual</label><input id="manualTime" type="datetime-local" step="1"><div class="actions"><button class="btn" onclick="fillPhoneTime()">Isi waktu HP</button><button class="btn primary" id="saveButton" onclick="saveManualTime()">Simpan waktu</button></div><div class="message" id="message" role="status" aria-live="polite"></div><div class="connection-note">Untuk menyimpan waktu meski ESP32 dimatikan, modul RTC DS1307 harus terdeteksi dan baterainya berfungsi. Tanpa RTC, waktu hanya tersimpan selama ESP32 mendapat daya.</div></section>
<div class="footer">Jam Digital Interaktif · ESP32-C3 Super Mini<br>GUI web responsif · Pengaturan tersimpan di memori perangkat</div>
</main>
<script>
'use strict';
const $ = id => document.getElementById(id);
const HTTP_TIMEOUT_MS = 8000;
let syncInProgress = false;
let pollingHandle = null;
let lastStatus = null;

function text(id, value) { $(id).textContent = value; }
function pad2(value) { return String(value).padStart(2, '0'); }
function localDateTimeValue(date) {
  return date.getFullYear() + '-' + pad2(date.getMonth() + 1) + '-' + pad2(date.getDate()) +
    'T' + pad2(date.getHours()) + ':' + pad2(date.getMinutes()) + ':' + pad2(date.getSeconds());
}
function fillPhoneTime() {
  $('manualTime').value = localDateTimeValue(new Date());
  showMessage('Form diisi dari waktu lokal HP/laptop.');
}
function showMessage(message, isError = false) {
  text('message', message || '');
  $('message').classList.toggle('error', isError);
}
function setConnection(connected) {
  text('connectionText', connected ? 'Terhubung' : 'Terputus');
  $('connectionDot').style.background = connected ? 'var(--good)' : 'var(--bad)';
}
function formatTime(hour, minute, second, format) {
  if (Number(format) === 12) {
    const period = hour >= 12 ? 'PM' : 'AM';
    const twelveHour = hour % 12 || 12;
    return { time: pad2(twelveHour) + ':' + pad2(minute) + ':' + pad2(second), period };
  }
  return { time: pad2(hour) + ':' + pad2(minute) + ':' + pad2(second), period: '' };
}
function renderStatus(status) {
  lastStatus = status;
  const clock = formatTime(status.hour, status.minute, status.second, status.format);
  text('clockText', clock.time);
  text('periodText', clock.period);
  text('dateText', status.tanggal || 'Tanggal belum tersedia');
  text('sourceText', status.source || 'Waktu perangkat');
  text('timeSource', status.source || 'Tidak diketahui');
  text('rtcStatus', status.rtc ? 'Terdeteksi' : 'Tidak terdeteksi');
  text('syncBadge', status.timeConfigured ? 'Waktu sudah disetel' : 'Perlu sinkronisasi');
  setConnection(true);

  $('format24').classList.toggle('active', Number(status.format) === 24);
  $('format12').classList.toggle('active', Number(status.format) === 12);
  $('modeClock').classList.toggle('active', status.mode === 'jam');
  $('modeDate').classList.toggle('active', status.mode === 'tanggal');
  $('modeAuto').classList.toggle('active', status.mode === 'auto');
  if (document.activeElement !== $('brightness')) {
    $('brightness').value = status.brightness;
    text('brightnessValue', status.brightness);
  }
  if (document.activeElement !== $('duration')) {
    $('duration').value = status.duration;
    text('durationValue', status.duration);
  }
}

async function api(path, parameters = {}, method = 'POST') {
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), HTTP_TIMEOUT_MS);
  const options = { method, cache: 'no-store', signal: controller.signal };
  let url = path;
  if (method === 'GET') {
    const query = new URLSearchParams(parameters).toString();
    if (query) url += '?' + query;
  } else {
    options.headers = { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' };
    options.body = new URLSearchParams(parameters).toString();
  }
  try {
    const response = await fetch(url, options);
    let data;
    try { data = await response.json(); }
    catch (_) { throw new Error('Balasan ESP32 tidak terbaca. Muat ulang halaman.'); }
    if (!response.ok) throw new Error(data.error || ('Permintaan gagal (' + response.status + ').'));
    return data;
  } catch (error) {
    if (error.name === 'AbortError') throw new Error('ESP32 tidak merespons. Periksa koneksi Wi-Fi.');
    throw error;
  } finally { clearTimeout(timeoutId); }
}

async function refreshStatus() {
  try { renderStatus(await api('/api/status', { _: Date.now() }, 'GET')); }
  catch (error) {
    setConnection(false);
    text('sourceText', 'Tidak bisa membaca status perangkat.');
  }
}

function localTimeParameters(date) {
  return {
    year: date.getFullYear(), month: date.getMonth() + 1, day: date.getDate(),
    hour: date.getHours(), minute: date.getMinutes(), second: date.getSeconds()
  };
}

async function sendTime(date, successFallback) {
  if (syncInProgress) return;
  syncInProgress = true;
  $('syncButton').disabled = true;
  $('saveButton').disabled = true;
  try {
    const status = await api('/api/time', localTimeParameters(date));
    renderStatus(status);
    showMessage(status.message || successFallback || 'Waktu berhasil diperbarui.');
    return true;
  } catch (error) {
    showMessage(error.message || 'Gagal menyimpan waktu.', true);
    await refreshStatus();
    return false;
  } finally {
    syncInProgress = false;
    $('syncButton').disabled = false;
    $('saveButton').disabled = false;
  }
}

async function syncFromPhone() {
  return sendTime(new Date(), 'Waktu disinkronkan dari HP/laptop.');
}

async function saveManualTime() {
  const value = $('manualTime').value;
  if (!value) { showMessage('Pilih tanggal dan waktu terlebih dahulu.', true); return; }
  const selected = new Date(value);
  if (Number.isNaN(selected.getTime())) { showMessage('Tanggal atau waktu tidak valid.', true); return; }
  await sendTime(selected, 'Waktu manual berhasil disimpan.');
}

async function updateSetting(path, values, successMessage) {
  try {
    const status = await api(path, values);
    renderStatus(status);
    if (successMessage) showMessage(successMessage);
  } catch (error) { showMessage(error.message, true); }
}
async function setFormat(format) {
  await updateSetting('/api/format', { format }, 'Format waktu berhasil disimpan.');
}
async function setDisplayMode(mode) {
  await updateSetting('/api/mode', { mode }, 'Mode display berhasil disimpan.');
}
async function setNumber(path, value) {
  await updateSetting(path, { value }, 'Pengaturan berhasil disimpan.');
}

(async function startPage() {
  fillPhoneTime();
  // Status awal memberi data perangkat, lalu sinkronisasi browser dilakukan sekali.
  await refreshStatus();
  await syncFromPhone();
  if (pollingHandle !== null) clearInterval(pollingHandle);
  pollingHandle = setInterval(refreshStatus, 1000);
})();
</script>
</body>
</html>
)rawliteral";

// ============================ MODE DISPLAY ============================
const char* modeToString(DisplayPreference mode) {
  switch (mode) {
    case DisplayPreference::CLOCK: return "jam";
    case DisplayPreference::DATE: return "tanggal";
    default: return "auto";
  }
}

bool parseDisplayMode(const String& value, DisplayPreference& output) {
  if (value == "jam") { output = DisplayPreference::CLOCK; return true; }
  if (value == "tanggal") { output = DisplayPreference::DATE; return true; }
  if (value == "auto") { output = DisplayPreference::AUTO; return true; }
  return false;
}

uint8_t displayHour(uint8_t hour) {
  if (timeFormat == TimeFormat::HOUR_24) return hour;
  const uint8_t converted = hour % 12;
  return converted == 0 ? 12 : converted;
}

void renderClockOnLed() {
  const DateTime now = currentDateTime();
  const uint8_t hour = displayHour(now.hour());
  const bool colonVisible = ((millis() / Config::CLOCK_REFRESH_MS) % 2U) == 0U;
  snprintf(clockBuffer, sizeof(clockBuffer), colonVisible ? "%02u:%02u" : "%02u %02u",
           static_cast<unsigned>(hour), static_cast<unsigned>(now.minute()));
  display.setTextAlignment(PA_CENTER);
  display.print(clockBuffer);
}

void startClockPage() {
  activeDisplayPage = DisplayPage::CLOCK;
  pageStartedMillis = millis();
  lastClockRefreshMillis = 0;
  display.displayClear();
  renderClockOnLed();
}

void startDatePage() {
  const DateTime now = currentDateTime();
  snprintf(dateBuffer, sizeof(dateBuffer), "%s, %u %s %u",
           hariNama(now.dayOfTheWeek()), static_cast<unsigned>(now.day()),
           bulanNama(now.month()), static_cast<unsigned>(now.year()));
  activeDisplayPage = DisplayPage::DATE;
  pageStartedMillis = millis();
  display.displayClear();
  display.displayText(dateBuffer, PA_LEFT, Config::DISPLAY_SCROLL_SPEED_MS, 0,
                      PA_SCROLL_LEFT, PA_SCROLL_LEFT);
}

void applyDisplayPreference() {
  if (displayPreference == DisplayPreference::DATE) startDatePage();
  else startClockPage();
}

void handleDisplayLoop() {
  const uint32_t nowMillis = millis();
  if (activeDisplayPage == DisplayPage::CLOCK) {
    if (nowMillis - lastClockRefreshMillis >= Config::CLOCK_REFRESH_MS) {
      lastClockRefreshMillis = nowMillis;
      renderClockOnLed();
    }
    if (displayPreference == DisplayPreference::AUTO &&
        nowMillis - pageStartedMillis >= static_cast<uint32_t>(displayDurationSeconds) * 1000UL) {
      startDatePage();
    }
    return;
  }

  const bool animationFinished = display.displayAnimate();
  if (!animationFinished) return;
  if (displayPreference == DisplayPreference::AUTO) startClockPage();
  else display.displayReset();
}

// ============================ API JSON ============================
void sendJsonError(uint16_t code, const String& message) {
  String json = "{\"error\":\"" + message + "\"}";
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(code, "application/json; charset=utf-8", json);
}

void sendStatus(const char* message = "") {
  const DateTime now = currentDateTime();
  char dateText[128];
  char time24[16];
  char json[640];
  snprintf(dateText, sizeof(dateText), "%s, %u %s %u",
           hariNama(now.dayOfTheWeek()), static_cast<unsigned>(now.day()),
           bulanNama(now.month()), static_cast<unsigned>(now.year()));
  snprintf(time24, sizeof(time24), "%02u:%02u:%02u",
           static_cast<unsigned>(now.hour()), static_cast<unsigned>(now.minute()),
           static_cast<unsigned>(now.second()));
  snprintf(json, sizeof(json),
    "{\"hour\":%u,\"minute\":%u,\"second\":%u,\"time24\":\"%s\",\"date\":\"%s\",\"tanggal\":\"%s\",\"mode\":\"%s\",\"format\":%u,\"brightness\":%u,\"duration\":%u,\"rtc\":%s,\"timeConfigured\":%s,\"source\":\"%s\",\"message\":\"%s\"}",
    static_cast<unsigned>(now.hour()), static_cast<unsigned>(now.minute()),
    static_cast<unsigned>(now.second()), time24, dateText, dateText,
    modeToString(displayPreference), static_cast<unsigned>(timeFormat),
    static_cast<unsigned>(brightness), static_cast<unsigned>(displayDurationSeconds),
    rtcAvailable ? "true" : "false",
    timeConfigured ? "true" : "false", timeSource, message);
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(Config::HTTP_OK, "application/json; charset=utf-8", json);
}

bool parseRequiredInteger(const char* name, int& value) {
  if (!server.hasArg(name) || server.arg(name).length() == 0) return false;
  const String raw = server.arg(name);
  char* end = nullptr;
  const long parsed = strtol(raw.c_str(), &end, 10);
  if (end == raw.c_str() || *end != '\0') return false;
  value = static_cast<int>(parsed);
  return true;
}

void handleTimeSet() {
  int year, month, day, hour, minute, second;
  const bool complete = parseRequiredInteger("year", year) &&
    parseRequiredInteger("month", month) && parseRequiredInteger("day", day) &&
    parseRequiredInteger("hour", hour) && parseRequiredInteger("minute", minute) &&
    parseRequiredInteger("second", second);
  if (!complete) {
    sendJsonError(Config::HTTP_BAD_REQUEST, "Data tanggal dan waktu tidak lengkap.");
    return;
  }

  String resultMessage;
  if (!saveDateTime(year, month, day, hour, minute, second,
                    "Waktu dari web browser", resultMessage)) {
    sendJsonError(Config::HTTP_BAD_REQUEST, resultMessage);
    return;
  }
  sendStatus(resultMessage.c_str());
}

void handleModeSet() {
  if (!server.hasArg("mode")) {
    sendJsonError(Config::HTTP_BAD_REQUEST, "Pilih mode display terlebih dahulu.");
    return;
  }
  DisplayPreference requestedMode;
  if (!parseDisplayMode(server.arg("mode"), requestedMode)) {
    sendJsonError(Config::HTTP_BAD_REQUEST, "Mode display tidak dikenal.");
    return;
  }
  displayPreference = requestedMode;
  preferences.putUChar("mode", static_cast<uint8_t>(displayPreference));
  applyDisplayPreference();
  sendStatus("Mode display berhasil disimpan.");
}

void handleFormatSet() {
  int formatValue;
  if (!parseRequiredInteger("format", formatValue) || (formatValue != 12 && formatValue != 24)) {
    sendJsonError(Config::HTTP_BAD_REQUEST, "Format harus 12 atau 24 jam.");
    return;
  }
  timeFormat = formatValue == 12 ? TimeFormat::HOUR_12 : TimeFormat::HOUR_24;
  preferences.putUChar("format", static_cast<uint8_t>(formatValue));
  if (activeDisplayPage == DisplayPage::CLOCK) renderClockOnLed();
  sendStatus("Format waktu berhasil disimpan.");
}

void handleBrightnessSet() {
  int value;
  if (!parseRequiredInteger("value", value)) {
    sendJsonError(Config::HTTP_BAD_REQUEST, "Nilai kecerahan tidak tersedia.");
    return;
  }
  brightness = static_cast<uint8_t>(constrain(value, Config::BRIGHTNESS_MIN, Config::BRIGHTNESS_MAX));
  display.setIntensity(brightness);
  preferences.putUChar("brightness", brightness);
  sendStatus("Kecerahan berhasil disimpan.");
}

void handleDurationSet() {
  int value;
  if (!parseRequiredInteger("value", value)) {
    sendJsonError(Config::HTTP_BAD_REQUEST, "Nilai durasi tidak tersedia.");
    return;
  }
  displayDurationSeconds = static_cast<uint16_t>(constrain(
    value, Config::DISPLAY_DURATION_MIN_SEC, Config::DISPLAY_DURATION_MAX_SEC));
  preferences.putUShort("duration", displayDurationSeconds);
  sendStatus("Durasi tampilan berhasil disimpan.");
}

// ============================ JARINGAN ============================
bool routerCredentialsConfigured() {
  return String(Config::WIFI_SSID).length() > 0 && String(Config::WIFI_SSID) != "NAMA_WIFI";
}

void initializeNetwork() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  const bool hotspotReady = WiFi.softAP(Config::AP_SSID, Config::AP_PASSWORD);
  Serial.println(hotspotReady ? "Hotspot setup aktif." : "Gagal membuat hotspot setup.");
  Serial.print("Wi-Fi hotspot: "); Serial.println(Config::AP_SSID);
  Serial.print("Password hotspot: "); Serial.println(Config::AP_PASSWORD);
  Serial.print("Alamat website hotspot: http://"); Serial.println(WiFi.softAPIP());

  if (routerCredentialsConfigured()) {
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
    wifiAttemptStarted = true;
    lastWifiAttemptMillis = millis();
    Serial.println("Koneksi router mulai dicoba di latar belakang.");
  } else {
    Serial.println("SSID router belum diisi. Gunakan hotspot JamDigital.");
  }
}

void handleNetworkLoop() {
  const uint32_t nowMillis = millis();
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !lastWifiConnected) {
    Serial.print("Router tersambung. IP LAN: http://");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(Config::MDNS_HOSTNAME)) {
      mdnsStarted = true;
      Serial.print("Nama lokal: http://"); Serial.print(Config::MDNS_HOSTNAME); Serial.println(".local");
    }
    configTzTime(Config::TIMEZONE_POSIX, Config::NTP_SERVER_1, Config::NTP_SERVER_2);
    ntpConfigured = true;
    lastNtpCheckMillis = 0;
    Serial.println("Sinkronisasi NTP dimulai.");
  }

  if (!connected && lastWifiConnected) {
    Serial.println("Koneksi router terputus; hotspot setup tetap aktif.");
  }
  lastWifiConnected = connected;

  if (routerCredentialsConfigured() && !connected &&
      (!wifiAttemptStarted || nowMillis - lastWifiAttemptMillis >= Config::WIFI_RETRY_MS)) {
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
    wifiAttemptStarted = true;
    lastWifiAttemptMillis = nowMillis;
    Serial.println("Mencoba menyambung ulang ke router...");
  }

  if (ntpConfigured && !ntpSynchronized && !userSetTimeThisBoot &&
      nowMillis - lastNtpCheckMillis >= Config::NTP_CHECK_MS) {
    lastNtpCheckMillis = nowMillis;
    const time_t epochNow = time(nullptr);
    if (epochNow >= static_cast<time_t>(Config::NTP_MIN_VALID_EPOCH)) {
      tm localTime{};
      if (localtime_r(&epochNow, &localTime) != nullptr &&
          tanggalWaktuValid(localTime.tm_year + 1900, localTime.tm_mon + 1,
            localTime.tm_mday, localTime.tm_hour, localTime.tm_min, localTime.tm_sec)) {
        setDateTimeFromTimeStruct(localTime, "NTP Wi-Fi (WIB)");
        ntpSynchronized = true;
        Serial.println("Waktu berhasil disinkronkan dari NTP.");
        applyDisplayPreference();
      }
    }
  }
}

// ============================ TOMBOL FISIK OPSIONAL ============================
void handlePhysicalButton() {
  const bool currentState = digitalRead(Config::PIN_BUTTON);
  const uint32_t nowMillis = millis();
  if (currentState == LOW && buttonPrevious == HIGH) buttonPressedMillis = nowMillis;
  if (currentState == HIGH && buttonPrevious == LOW &&
      nowMillis - buttonPressedMillis >= Config::BUTTON_DEBOUNCE_MS) {
    if (displayPreference == DisplayPreference::CLOCK) displayPreference = DisplayPreference::DATE;
    else if (displayPreference == DisplayPreference::DATE) displayPreference = DisplayPreference::AUTO;
    else displayPreference = DisplayPreference::CLOCK;
    preferences.putUChar("mode", static_cast<uint8_t>(displayPreference));
    applyDisplayPreference();
  }
  buttonPrevious = currentState;
}

// ============================ SETUP ============================
void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  pinMode(Config::PIN_BUTTON, INPUT_PULLUP);

  preferences.begin("jaminteraktif", false);
  const uint8_t savedMode = preferences.getUChar("mode", static_cast<uint8_t>(DisplayPreference::AUTO));
  displayPreference = savedMode <= static_cast<uint8_t>(DisplayPreference::AUTO)
    ? static_cast<DisplayPreference>(savedMode) : DisplayPreference::AUTO;
  brightness = static_cast<uint8_t>(constrain(
    preferences.getUChar("brightness", Config::BRIGHTNESS_DEFAULT),
    Config::BRIGHTNESS_MIN, Config::BRIGHTNESS_MAX));
  displayDurationSeconds = static_cast<uint16_t>(constrain(
    preferences.getUShort("duration", Config::DISPLAY_DURATION_DEFAULT_SEC),
    Config::DISPLAY_DURATION_MIN_SEC, Config::DISPLAY_DURATION_MAX_SEC));
  const uint8_t savedFormat = preferences.getUChar("format", static_cast<uint8_t>(TimeFormat::HOUR_24));
  timeFormat = savedFormat == 12 ? TimeFormat::HOUR_12 : TimeFormat::HOUR_24;

  Wire.begin(Config::PIN_SDA, Config::PIN_SCL);
  rtcAvailable = rtc.begin();
  const DateTime buildTime(F(__DATE__), F(__TIME__));
  if (rtcAvailable) {
    const DateTime rtcTime = rtc.now();
    if (!rtc.isrunning() || !tahunValid(rtcTime.year())) {
      rtc.adjust(buildTime);
      setFallbackTime(buildTime);
      timeSource = "Waktu kompilasi (belum disinkronkan)";
      timeConfigured = false;
      Serial.println("RTC terdeteksi tetapi waktunya belum valid; tunggu sinkronisasi web/NTP.");
    } else {
      setFallbackTime(rtcTime);
      timeSource = "RTC DS1307";
      timeConfigured = true;
      Serial.println("RTC DS1307 terdeteksi dan waktu terbaca.");
    }
  } else {
    setFallbackTime(buildTime);
    timeSource = "Waktu kompilasi (belum disinkronkan)";
    timeConfigured = false;
    Serial.println("RTC DS1307 tidak terdeteksi. Waktu akan disetel dari web atau NTP.");
  }

  SPI.begin(Config::PIN_CLK, -1, Config::PIN_DIN, Config::PIN_CS);
  display.begin();
  display.setIntensity(brightness);
  display.displayClear();
  display.setTextAlignment(PA_CENTER);
  display.print("BOOT");

  initializeNetwork();

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.send_P(Config::HTTP_OK, "text/html; charset=utf-8", WEB_PAGE);
  });
  server.on("/api/status", HTTP_GET, []() { sendStatus(); });
  server.on("/api/time", HTTP_POST, handleTimeSet);
  server.on("/api/mode", HTTP_POST, handleModeSet);
  server.on("/api/format", HTTP_POST, handleFormatSet);
  server.on("/api/brightness", HTTP_POST, handleBrightnessSet);
  server.on("/api/duration", HTTP_POST, handleDurationSet);
  server.onNotFound([]() {
    sendJsonError(Config::HTTP_NOT_FOUND, "Alamat API tidak ditemukan. Muat ulang halaman.");
  });
  server.begin();
  Serial.println("Web server aktif.");
  Serial.println("Buka http://192.168.4.1 setelah terhubung ke hotspot JamDigital.");

  applyDisplayPreference();
}

// ============================ LOOP NON-BLOCKING ============================
void loop() {
  server.handleClient();
  handleNetworkLoop();
  handlePhysicalButton();
  handleDisplayLoop();

  const uint32_t nowMillis = millis();
  if (!rtcAvailable && nowMillis - lastRtcProbeMillis >= Config::RTC_RETRY_MS) {
    lastRtcProbeMillis = nowMillis;
    if (rtc.begin()) {
      rtcAvailable = true;
      const DateTime currentFallback = fallbackTimeNow();
      rtc.adjust(currentFallback);
      setFallbackTime(currentFallback);
      if (timeConfigured) timeSource = "RTC DS1307";
      Serial.println("RTC DS1307 baru terdeteksi; waktu saat ini ditulis ke RTC.");
    }
  }
}
