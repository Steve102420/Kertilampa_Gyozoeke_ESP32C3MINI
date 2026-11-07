#include <Arduino.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_data_cert_x509_crt_bundle_bin_end");

namespace
{
constexpr uint8_t LIGHT_GPIO = 8;
constexpr uint8_t LIGHT_PWM_CHANNEL = 0;
constexpr uint32_t LIGHT_PWM_FREQUENCY = 5000;
constexpr uint8_t LIGHT_PWM_RESOLUTION = 8;
constexpr uint16_t LIGHT_PWM_MAX = (1 << LIGHT_PWM_RESOLUTION) - 1;
constexpr uint32_t LIGHT_FULL_RAMP_MS = 2000;
constexpr char AP_SSID[] = "Kertilampa-Setup";
constexpr char AP_PASSWORD[] = "KertiLampa4";
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.google.com";
constexpr char DEFAULT_TIME_ZONE[] = "Europe/Budapest";
constexpr char OTA_RELEASES_API[] =
    "https://api.github.com/repos/Steve102420/Kertilampa_Gyozoeke_ESP32C3MINI/releases/latest";
constexpr char OTA_RELEASE_DOWNLOAD[] =
    "https://github.com/Steve102420/Kertilampa_Gyozoeke_ESP32C3MINI/releases/latest/download/firmware.bin";
constexpr char OTA_TAG_PREFIX[] = "ota-";
constexpr uint32_t OTA_HTTP_TIMEOUT_MS = 15000;
constexpr uint32_t OTA_TASK_STACK_SIZE = 8192;

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "development"
#endif

struct TimeZoneOption
{
  const char *id;
  const char *label;
  const char *posix;
};

constexpr TimeZoneOption TIME_ZONES[] = {
    {"UTC", "UTC", "UTC0"},
    {"Europe/London", "London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Budapest", "Budapest", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "Athén", "EET-2EEST,M3.5.0,M10.5.0/3"},
    {"America/New_York", "New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "Phoenix", "MST7"},
    {"Asia/Kolkata", "Kolkata", "IST-5:30"},
    {"Asia/Bangkok", "Bangkok", "ICT-7"},
    {"Asia/Shanghai", "Sanghaj", "CST-8"},
    {"Asia/Tokyo", "Tokió", "JST-9"},
    {"Australia/Sydney", "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"}};

WebServer server(80);
Preferences preferences;

String savedSsid;
String onTime = "06:00";
String offTime = "22:00";
String timeZoneId = DEFAULT_TIME_ZONE;
bool lightOn = false;
uint8_t brightnessPercent = 100;
float currentBrightness = 0;
float rampStartBrightness = 0;
float rampTargetBrightness = 0;
unsigned long rampStartedAt = 0;
unsigned long rampDuration = 0;
bool ntpConfigured = false;
bool scheduleCheckedAfterBoot = false;
bool otaStartupCheckDone = false;
int lastOtaCheckDate = -1;
volatile bool otaCheckInProgress = false;
int lastNtpSyncDate = -1;
unsigned long lastReconnectAttempt = 0;

const TimeZoneOption *findTimeZone(const String &id)
{
  for (const TimeZoneOption &zone : TIME_ZONES)
  {
    if (id == zone.id)
      return &zone;
  }
  return nullptr;
}

void applyTimeZone()
{
  const TimeZoneOption *zone = findTimeZone(timeZoneId);
  if (zone == nullptr)
    zone = findTimeZone(DEFAULT_TIME_ZONE);
  setenv("TZ", zone->posix, 1);
  tzset();
}

String jsonEscape(const String &value)
{
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i)
  {
    const char c = value[i];
    if (c == '"' || c == '\\')
    {
      escaped += '\\';
      escaped += c;
    }
    else if (static_cast<uint8_t>(c) < 0x20)
    {
      escaped += ' ';
    }
    else
    {
      escaped += c;
    }
  }
  return escaped;
}

bool validTime(const String &value)
{
  if (value.length() != 5 || value[2] != ':')
    return false;
  if (!isDigit(value[0]) || !isDigit(value[1]) ||
      !isDigit(value[3]) || !isDigit(value[4]))
    return false;

  const int hour = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

int minutesSinceMidnight(const String &value)
{
  return value.substring(0, 2).toInt() * 60 + value.substring(3, 5).toInt();
}

bool scheduleWantsLightOn(const tm &timeInfo)
{
  const int now = timeInfo.tm_hour * 60 + timeInfo.tm_min;
  const int start = minutesSinceMidnight(onTime);
  const int end = minutesSinceMidnight(offTime);
  if (start < end)
    return now >= start && now < end;
  if (start > end)
    return now >= start || now < end;
  return false;
}

void saveLightState()
{
  preferences.putBool("lightOn", lightOn);
}

uint8_t brightnessToPwm(uint8_t percent)
{
  const float normalized = static_cast<float>(percent) / 100.0f;
  const float logarithmicScale = (powf(10.0f, normalized) - 1.0f) / 9.0f;
  return static_cast<uint8_t>(roundf(logarithmicScale * LIGHT_PWM_MAX));
}

void writeLightPwm(uint8_t duty)
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LIGHT_GPIO, duty);
#else
  ledcWrite(LIGHT_PWM_CHANNEL, duty);
#endif
}

void configureLightPwm()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(LIGHT_GPIO, LIGHT_PWM_FREQUENCY, LIGHT_PWM_RESOLUTION))
    Serial.println("Hiba: nem sikerült beállítani a LED PWM-kimenetet.");
#else
  ledcSetup(LIGHT_PWM_CHANNEL, LIGHT_PWM_FREQUENCY, LIGHT_PWM_RESOLUTION);
  ledcAttachPin(LIGHT_GPIO, LIGHT_PWM_CHANNEL);
#endif
}

void updateLightOutput()
{
  if (rampDuration == 0)
  {
    currentBrightness = rampTargetBrightness;
  }
  else
  {
    const unsigned long elapsed = millis() - rampStartedAt;
    if (elapsed >= rampDuration)
    {
      currentBrightness = rampTargetBrightness;
      rampDuration = 0;
    }
    else
    {
      const float progress = static_cast<float>(elapsed) / rampDuration;
      currentBrightness = rampStartBrightness +
                          (rampTargetBrightness - rampStartBrightness) * progress;
    }
  }

  const uint8_t logicalDuty = static_cast<uint8_t>(roundf(currentBrightness));
  writeLightPwm(LIGHT_PWM_MAX - logicalDuty);
}

void rampLightToCurrentTarget()
{
  rampStartBrightness = currentBrightness;
  rampTargetBrightness = lightOn ? brightnessToPwm(brightnessPercent) : 0;
  rampStartedAt = millis();
  rampDuration = static_cast<unsigned long>(
      roundf(fabsf(rampTargetBrightness - rampStartBrightness) *
             LIGHT_FULL_RAMP_MS / LIGHT_PWM_MAX));
  if (rampDuration == 0)
    currentBrightness = rampTargetBrightness;
}

void setLight(bool turnOn, bool persist = true)
{
  lightOn = turnOn;
  rampLightToCurrentTarget();
  if (persist)
    saveLightState();
}

bool getLocalTimeSafe(tm &timeInfo)
{
  return getLocalTime(&timeInfo, 0);
}

String formattedLocalTime()
{
  tm timeInfo{};
  if (!getLocalTimeSafe(timeInfo))
    return "";

  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
  return String(buffer);
}

void startNtp()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  const TimeZoneOption *zone = findTimeZone(timeZoneId);
  if (zone == nullptr)
  {
    Serial.println("Hiba: érvénytelen időzóna-beállítás.");
    return;
  }
  configTzTime(zone->posix, NTP_SERVER_1, NTP_SERVER_2);
  ntpConfigured = true;
}

void sendStatus()
{
  const bool connected = WiFi.status() == WL_CONNECTED;
  const String currentSsid = connected ? WiFi.SSID() : "";
  const String ip = connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  const String localTime = formattedLocalTime();
  String response = "{";
  response += "\"lightOn\":" + String(lightOn ? "true" : "false");
  response += ",\"brightness\":" + String(brightnessPercent);
  response += ",\"onTime\":\"" + jsonEscape(onTime) + "\"";
  response += ",\"offTime\":\"" + jsonEscape(offTime) + "\"";
  response += ",\"wifiConnected\":" + String(connected ? "true" : "false");
  response += ",\"savedSsid\":\"" + jsonEscape(savedSsid) + "\"";
  response += ",\"currentSsid\":\"" + jsonEscape(currentSsid) + "\"";
  response += ",\"ip\":\"" + jsonEscape(ip) + "\"";
  response += ",\"timeZone\":\"" + jsonEscape(timeZoneId) + "\"";
  response += ",\"ntpSynced\":" + String(localTime.length() > 0 ? "true" : "false");
  response += ",\"localTime\":\"" + jsonEscape(localTime) + "\"}";
  server.send(200, "application/json; charset=utf-8", response);
}

void handleSaveTimeZone()
{
  const String requestedTimeZone = server.arg("timeZone");
  if (findTimeZone(requestedTimeZone) == nullptr)
  {
    server.send(400, "application/json; charset=utf-8",
                "{\"error\":\"Válassz egy érvényes időzónát.\"}");
    return;
  }

  timeZoneId = requestedTimeZone;
  preferences.putString("timeZone", timeZoneId);
  applyTimeZone();
  ntpConfigured = false;
  if (WiFi.status() == WL_CONNECTED)
    startNtp();
  server.send(200, "application/json; charset=utf-8", "{\"saved\":true}");
}

void handleScan()
{
  const int networkCount = WiFi.scanNetworks();
  if (networkCount < 0)
  {
    server.send(500, "application/json; charset=utf-8",
                "{\"error\":\"A Wi-Fi keresés nem sikerült.\"}");
    return;
  }

  String response = "[";
  for (int i = 0; i < networkCount; ++i)
  {
    if (i > 0)
      response += ',';
    response += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" +
                String(WiFi.RSSI(i)) + ",\"secure\":" +
                String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  response += ']';
  WiFi.scanDelete();
  server.send(200, "application/json; charset=utf-8", response);
}

void handleSaveSchedule()
{
  const String requestedOn = server.arg("onTime");
  const String requestedOff = server.arg("offTime");
  if (!validTime(requestedOn) || !validTime(requestedOff))
  {
    server.send(400, "application/json; charset=utf-8",
                "{\"error\":\"Érvénytelen időpont. HH:MM formátum szükséges.\"}");
    return;
  }

  onTime = requestedOn;
  offTime = requestedOff;
  preferences.putString("onTime", onTime);
  preferences.putString("offTime", offTime);
  server.send(200, "application/json; charset=utf-8", "{\"saved\":true}");
}

void handleSetLight()
{
  const String value = server.arg("on");
  if (value != "0" && value != "1")
  {
    server.send(400, "application/json; charset=utf-8",
                "{\"error\":\"Érvénytelen GPIO állapot.\"}");
    return;
  }
  setLight(value == "1");
  server.send(200, "application/json; charset=utf-8", "{\"saved\":true}");
}

void handleSetBrightness()
{
  const String value = server.arg("brightness");
  const int requestedBrightness = value.toInt();
  if (value.isEmpty() || String(requestedBrightness) != value ||
      requestedBrightness < 0 || requestedBrightness > 100)
  {
    server.send(400, "application/json; charset=utf-8",
                "{\"error\":\"A fényerő értéke 0 és 100 közötti egész szám legyen.\"}");
    return;
  }

  const String save = server.arg("save");
  if (save != "0" && save != "1")
  {
    server.send(400, "application/json; charset=utf-8",
                "{\"error\":\"Érvénytelen fényerő-mentési beállítás.\"}");
    return;
  }

  brightnessPercent = static_cast<uint8_t>(requestedBrightness);
  rampLightToCurrentTarget();
  if (save == "1")
    preferences.putUChar("brightness", brightnessPercent);
  server.send(200, "application/json; charset=utf-8",
              save == "1" ? "{\"saved\":true}" : "{\"saved\":false}");
}

void handleConnectWifi()
{
  const String requestedSsid = server.arg("ssid");
  const String password = server.arg("password");
  if (requestedSsid.isEmpty())
  {
    server.send(400, "application/json; charset=utf-8",
                "{\"error\":\"Válassz ki egy Wi-Fi hálózatot.\"}");
    return;
  }

  WiFi.begin(requestedSsid.c_str(), password.c_str());
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000)
  {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    WiFi.disconnect();
    server.send(400, "application/json; charset=utf-8",
                "{\"error\":\"Nem sikerült csatlakozni. Ellenőrizd a jelszót és próbáld újra.\"}");
    return;
  }

  savedSsid = requestedSsid;
  preferences.putString("ssid", savedSsid);
  preferences.putString("password", password);
  startNtp();
  server.send(200, "application/json; charset=utf-8", "{\"connected\":true}");
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="hu">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Kerti lámpa vezérlő</title>
  <style>
    :root{color-scheme:dark;font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;color:#e7eee9;background:#080e0c;font-synthesis:none;text-rendering:optimizeLegibility}
    *{box-sizing:border-box}
    body{max-width:760px;margin:0 auto;padding:52px 22px 40px;background:radial-gradient(ellipse at 50% 0%,#153728 0,transparent 58%)}
    h1{margin:0 0 26px;font-size:clamp(1.8rem,5vw,2.5rem);font-weight:720;letter-spacing:-.045em;line-height:1.15}
    h1:before{content:"";display:inline-block;width:11px;height:11px;margin:0 12px 5px 0;border-radius:50%;background:#75e0a1;box-shadow:0 0 18px #75e0a188}
    section{margin:14px 0;padding:24px 26px;border:1px solid #293a32;border-radius:18px;background:linear-gradient(145deg,#141e19,#101713 72%);box-shadow:0 12px 34px #0003}
    h2{margin:0 0 18px;color:#dce9e0;font-size:1.05rem;font-weight:650;letter-spacing:.01em}
    button,input,select{font:inherit;color:#e7eee9}
    button{margin:4px 6px 4px 0;padding:10px 16px;border:1px solid #65d995;border-radius:10px;background:#65d995;color:#0b1710;font-weight:650;cursor:pointer;transition:background .18s,border-color .18s,transform .18s}
    button:hover{border-color:#8debb1;background:#8debb1;transform:translateY(-1px)}
    button.secondary{border-color:#34483c;background:#202d25;color:#dce9e0}
    button.secondary:hover{border-color:#65d995;background:#263a2d}
    button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid #75e0a1;outline-offset:3px}
    label{display:block;margin:16px 0 7px;color:#bccbc1;font-size:.9rem;font-weight:550}
    input:not([type=range]),select{width:100%;min-height:44px;padding:9px 12px;border:1px solid #35443b;border-radius:10px;background:#0c130f}
    input[type=range]{width:100%;height:8px;margin:14px 0 6px;padding:0;accent-color:#75e0a1;cursor:pointer}
    .row{display:flex;gap:14px;flex-wrap:wrap;align-items:center}
    .row>div{flex:1;min-width:180px}
    .status{display:inline-flex;align-items:center;min-height:38px;margin:0 0 12px;padding:7px 12px;border:1px solid #34483c;border-radius:999px;background:#1a2920;color:#9be9b4;font-size:.98rem;font-weight:650}
    .muted{color:#91a197;font-size:.88rem;line-height:1.6}
    #brightnessValue{color:#9be9b4;font-variant-numeric:tabular-nums}
    #clockStatus,#wifiStatus{margin:10px 0}
    a{color:#9be9b4;text-decoration-color:#527f62;text-underline-offset:3px}
    a:hover{color:#c0f5d0}
    #message{min-height:1.5em;margin:18px 4px;color:#9be9b4;font-size:.92rem}
    @media(max-width:520px){body{padding:32px 14px 28px}section{padding:20px 18px;border-radius:15px}.row>div{min-width:100%}button{padding:10px 13px}}
    @media(prefers-reduced-motion:reduce){*,*:before,*:after{scroll-behavior:auto!important;transition:none!important}}
  </style>
</head>
<body>
  <h1>Kerti lámpa vezérlő</h1>
  <section>
    <h2>Lámpa állapota</h2>
    <p class="status" id="lightStatus">Betöltés…</p>
    <button onclick="setLight(true)">Bekapcsolás</button>
    <button class="secondary" onclick="setLight(false)">Kikapcsolás</button>
    <label for="brightness">Fényerő: <span id="brightnessValue">100%</span></label>
    <input id="brightness" type="range" min="0" max="100" step="1" value="100">
    <p class="muted">Logaritmikus skála; a fényerő változtatása fokozatos, teljes tartományon kb. 2 másodperc.</p>
    <p class="muted" id="clockStatus"></p>
  </section>
  <section>
    <h2>Napi időzítés</h2>
    <div class="row">
      <div><label for="onTime">Bekapcsolás időpontja</label><input id="onTime" type="time"></div>
      <div><label for="offTime">Kikapcsolás időpontja</label><input id="offTime" type="time"></div>
    </div>
    <p><button onclick="saveSchedule()">Időzítés mentése</button></p>
    <label for="timeZone">Időzóna</label>
    <select id="timeZone">
      <option value="UTC">UTC</option>
      <option value="Europe/London">London</option>
      <option value="Europe/Budapest">Budapest</option>
      <option value="Europe/Athens">Athén</option>
      <option value="America/New_York">New York</option>
      <option value="America/Chicago">Chicago</option>
      <option value="America/Denver">Denver</option>
      <option value="America/Los_Angeles">Los Angeles</option>
      <option value="America/Phoenix">Phoenix</option>
      <option value="Asia/Kolkata">Kolkata</option>
      <option value="Asia/Bangkok">Bangkok</option>
      <option value="Asia/Shanghai">Sanghaj</option>
      <option value="Asia/Tokyo">Tokió</option>
      <option value="Australia/Sydney">Sydney</option>
      <option value="Pacific/Auckland">Auckland</option>
    </select>
    <button onclick="saveTimeZone()">Időzóna mentése</button>
    <p class="muted">A nyári/téli időszámítás az adott időzóna szabályai szerint automatikus.</p>
  </section>
  <section>
    <h2>Wi-Fi hálózat</h2>
    <p class="muted" id="wifiStatus">Betöltés…</p>
    <p class="muted">Helyi cím: <a href="http://kertilampa.local">http://kertilampa.local</a></p>
    <button class="secondary" onclick="scanWifi()">Hálózatok keresése</button>
    <label for="networks">Talált hálózatok</label>
    <select id="networks"><option value="">Előbb keress hálózatot</option></select>
    <label for="password">Wi-Fi jelszó</label>
    <input id="password" type="password" autocomplete="current-password">
    <p><button onclick="connectWifi()">Csatlakozás és mentés</button></p>
  </section>
  <p id="message" role="status"></p>
<script>
const message = document.getElementById('message');
const brightnessSlider = document.getElementById('brightness');
let brightnessTimer;
let brightnessQueue = Promise.resolve();
async function request(url, options) {
  const response = await fetch(url, options);
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || 'A művelet nem sikerült.');
  return result;
}
function showError(error) { message.textContent = error.message; }
function queueBrightness(save) {
  const value = brightnessSlider.value;
  brightnessQueue = brightnessQueue.then(async () => {
    const body = new URLSearchParams({brightness:value,save:save?'1':'0'});
    await request('/api/brightness', {method:'POST',body});
    if (save) message.textContent = 'A fényerő elmentve.';
  }).catch(showError);
}
brightnessSlider.addEventListener('input', () => {
  document.getElementById('brightnessValue').textContent = brightnessSlider.value + '%';
  clearTimeout(brightnessTimer);
  brightnessTimer = setTimeout(() => queueBrightness(false), 150);
});
brightnessSlider.addEventListener('change', () => {
  clearTimeout(brightnessTimer);
  queueBrightness(true);
});
async function refresh() {
  try {
    const s = await request('/api/status');
    document.getElementById('lightStatus').textContent = 'A lámpa jelenleg ' + (s.lightOn ? 'BE van kapcsolva' : 'KI van kapcsolva');
    if (document.activeElement !== brightnessSlider) {
      brightnessSlider.value = s.brightness;
      document.getElementById('brightnessValue').textContent = s.brightness + '%';
    }
    if (document.activeElement !== document.getElementById('onTime')) document.getElementById('onTime').value = s.onTime;
    if (document.activeElement !== document.getElementById('offTime')) document.getElementById('offTime').value = s.offTime;
    if (document.activeElement !== document.getElementById('timeZone')) document.getElementById('timeZone').value = s.timeZone;
    document.getElementById('clockStatus').textContent = s.ntpSynced ? 'Helyi idő (' + s.timeZone + '): ' + s.localTime + ' (Szinkronizálva)' : 'NTP időszinkronra vár.';
    document.getElementById('wifiStatus').textContent = s.wifiConnected
      ? 'Csatlakozva: ' + s.currentSsid + ' — IP-cím: ' + s.ip
      : 'Beállító hálózat: Kertilampa-Setup — IP-cím: ' + s.ip + (s.savedSsid ? ' — Mentett hálózat: ' + s.savedSsid : '');
  } catch (error) { showError(error); }
}
async function setLight(on) {
  try { await request('/api/led', {method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'on='+(on?'1':'0')}); message.textContent='A lámpa állapota elmentve.'; await refresh(); }
  catch (error) { showError(error); }
}
async function saveSchedule() {
  try {
    const body = new URLSearchParams({onTime:document.getElementById('onTime').value,offTime:document.getElementById('offTime').value});
    await request('/api/settings', {method:'POST',body});
    message.textContent='Az időzítés elmentve.';
  } catch (error) { showError(error); }
}
async function saveTimeZone() {
  try {
    const body = new URLSearchParams({timeZone:document.getElementById('timeZone').value});
    await request('/api/timezone', {method:'POST',body});
    message.textContent='Az időzóna elmentve. Az NTP-szinkronizálás újraindult.';
    await refresh();
  } catch (error) { showError(error); }
}
async function scanWifi() {
  const list = document.getElementById('networks');
  list.replaceChildren(new Option('Keresés…',''));
  try {
    const networks = await request('/api/scan');
    list.replaceChildren();
    if (!networks.length) list.add(new Option('Nem található hálózat',''));
    for (const n of networks) {
      const option = new Option(n.ssid + ' (' + n.rssi + ' dBm)' + (n.secure ? ' 🔒' : ' — nyílt'), n.ssid);
      list.add(option);
    }
    message.textContent=networks.length ? 'Hálózatkeresés kész.' : 'Nem található Wi-Fi hálózat.';
  } catch (error) { showError(error); }
}
async function connectWifi() {
  try {
    const body = new URLSearchParams({ssid:document.getElementById('networks').value,password:document.getElementById('password').value});
    await request('/api/wifi', {method:'POST',body});
    message.textContent='Wi-Fi kapcsolat létrejött, a beállítás elmentve.';
    document.getElementById('password').value='';
    await refresh();
  } catch (error) { showError(error); }
}
refresh();
setInterval(refresh,5000);
</script>
</body>
</html>
)HTML";

void configureRoutes()
{
  server.on("/", HTTP_GET, []()
            { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/settings", HTTP_POST, handleSaveSchedule);
  server.on("/api/timezone", HTTP_POST, handleSaveTimeZone);
  server.on("/api/led", HTTP_POST, handleSetLight);
  server.on("/api/brightness", HTTP_POST, handleSetBrightness);
  server.on("/api/wifi", HTTP_POST, handleConnectWifi);
  server.onNotFound([]()
                    { server.send(404, "text/plain; charset=utf-8", "Az oldal nem található."); });
}

void checkDailySchedule()
{
  tm timeInfo{};
  if (!getLocalTimeSafe(timeInfo))
    return;

  if (!scheduleCheckedAfterBoot)
  {
    setLight(scheduleWantsLightOn(timeInfo));
    scheduleCheckedAfterBoot = true;
  }

  const int dateKey = timeInfo.tm_year * 366 + timeInfo.tm_yday;
  char currentTime[6];
  strftime(currentTime, sizeof(currentTime), "%H:%M", &timeInfo);
  if (onTime == currentTime && preferences.getInt("onDay", -1) != dateKey)
  {
    setLight(true);
    preferences.putInt("onDay", dateKey);
  }
  if (offTime == currentTime && preferences.getInt("offDay", -1) != dateKey)
  {
    setLight(false);
    preferences.putInt("offDay", dateKey);
  }

  if (timeInfo.tm_hour >= 2 && lastNtpSyncDate != dateKey &&
      WiFi.status() == WL_CONNECTED)
  {
    startNtp();
    lastNtpSyncDate = dateKey;
  }
}

void reconnectWifiIfNeeded()
{
  if (savedSsid.isEmpty() || WiFi.status() == WL_CONNECTED ||
      millis() - lastReconnectAttempt < 30000)
    return;

  lastReconnectAttempt = millis();
  WiFi.begin(savedSsid.c_str(), preferences.getString("password", "").c_str());
}

String extractJsonStringField(const String &json, const char *field)
{
  String key = "\"" + String(field) + "\"";
  int position = json.indexOf(key);
  if (position < 0)
    return "";

  position = json.indexOf(':', position + key.length());
  if (position < 0)
    return "";
  ++position;
  while (position < static_cast<int>(json.length()) && isspace(json[position]))
    ++position;
  if (position >= static_cast<int>(json.length()) || json[position] != '"')
    return "";

  String value;
  for (++position; position < static_cast<int>(json.length()); ++position)
  {
    const char c = json[position];
    if (c == '"')
      return value;
    if (c == '\\')
      return "";
    value += c;
  }
  return "";
}

bool validOtaTag(const String &tag)
{
  if (!tag.startsWith(OTA_TAG_PREFIX) ||
      tag.length() != strlen(OTA_TAG_PREFIX) + 40)
    return false;

  for (size_t i = strlen(OTA_TAG_PREFIX); i < tag.length(); ++i)
  {
    const char c = tag[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

void performOtaCheck()
{
  WiFiClientSecure secureClient;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  secureClient.setCACertBundle(rootca_crt_bundle_start,
                               rootca_crt_bundle_end - rootca_crt_bundle_start);
#else
  secureClient.setCACertBundle(rootca_crt_bundle_start);
#endif

  HTTPClient http;
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(secureClient, OTA_RELEASES_API))
  {
    Serial.println("OTA hiba: nem sikerült elindítani a GitHub verzióellenőrzést.");
    return;
  }

  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK)
  {
    Serial.printf("OTA hiba: a GitHub verzióellenőrzés HTTP-kódja: %d\n",
                  responseCode);
    http.end();
    return;
  }

  const String response = http.getString();
  http.end();
  const String latestTag = extractJsonStringField(response, "tag_name");
  if (!validOtaTag(latestTag))
  {
    Serial.println("OTA hiba: hiányzó vagy érvénytelen GitHub kiadási verzió.");
    return;
  }

  const String latestVersion = latestTag.substring(strlen(OTA_TAG_PREFIX));
  if (latestVersion == FIRMWARE_VERSION)
  {
    Serial.printf("OTA: a firmware naprakész (%s).\n", FIRMWARE_VERSION);
    return;
  }

  Serial.printf("OTA: új verzió érhető el (%s); letöltés indul.\n",
                latestVersion.c_str());
  HTTPUpdate updater;
  updater.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  updater.rebootOnUpdate(true);
  const t_httpUpdate_return result =
      updater.update(secureClient, OTA_RELEASE_DOWNLOAD, FIRMWARE_VERSION);
  if (result == HTTP_UPDATE_FAILED)
  {
    Serial.printf("OTA hiba: %s\n", updater.getLastErrorString().c_str());
  }
  else if (result == HTTP_UPDATE_NO_UPDATES)
  {
    Serial.println("OTA: a GitHub nem kínál új firmware-t.");
  }
}

void otaCheckTask(void *)
{
  performOtaCheck();
  otaCheckInProgress = false;
  vTaskDelete(nullptr);
}

bool startOtaCheck()
{
  if (otaCheckInProgress)
    return false;

  otaCheckInProgress = true;
  if (xTaskCreate(otaCheckTask, "ota-check", OTA_TASK_STACK_SIZE, nullptr, 1,
                  nullptr) != pdPASS)
  {
    otaCheckInProgress = false;
    Serial.println("OTA hiba: nem sikerült elindítani a frissítési feladatot.");
    return false;
  }
  return true;
}

void checkOtaSchedule()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  tm timeInfo{};
  if (!getLocalTimeSafe(timeInfo) || timeInfo.tm_year < 120)
    return;

  const int dateKey = timeInfo.tm_year * 366 + timeInfo.tm_yday;
  if (!otaStartupCheckDone)
  {
    if (startOtaCheck())
    {
      otaStartupCheckDone = true;
      if (timeInfo.tm_hour >= 3)
        lastOtaCheckDate = dateKey;
    }
    return;
  }

  if (timeInfo.tm_hour >= 3 && lastOtaCheckDate != dateKey &&
      startOtaCheck())
    lastOtaCheckDate = dateKey;
}
} // namespace

void setup()
{
  Serial.begin(115200);
  pinMode(LIGHT_GPIO, OUTPUT);

  preferences.begin("kertilampa", false);
  savedSsid = preferences.getString("ssid", "");
  onTime = preferences.getString("onTime", "06:00");
  offTime = preferences.getString("offTime", "22:00");
  timeZoneId = preferences.getString("timeZone", DEFAULT_TIME_ZONE);
  brightnessPercent = preferences.getUChar("brightness", 100);
  if (brightnessPercent > 100)
    brightnessPercent = 100;
  if (findTimeZone(timeZoneId) == nullptr)
    timeZoneId = DEFAULT_TIME_ZONE;
  lightOn = preferences.getBool("lightOn", false);
  if (!validTime(onTime))
    onTime = "06:00";
  if (!validTime(offTime))
    offTime = "22:00";
  configureLightPwm();
  currentBrightness = lightOn ? brightnessToPwm(brightnessPercent) : 0;
  rampStartBrightness = currentBrightness;
  rampTargetBrightness = currentBrightness;
  updateLightOutput();

  applyTimeZone();
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD))
    Serial.println("Hiba: nem sikerült elindítani a beállító Wi-Fi hálózatot.");

  if (!MDNS.begin("kertilampa"))
    Serial.println("Hiba: nem sikerült elindítani a kertilampa.local mDNS címet.");
  else
    MDNS.addService("http", "tcp", 80);

  if (!savedSsid.isEmpty())
    WiFi.begin(savedSsid.c_str(), preferences.getString("password", "").c_str());

  configureRoutes();
  server.begin();

  Serial.print("Beállító hálózat: ");
  Serial.println(AP_SSID);
  Serial.print("Beállító oldal: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("Helyi cím: http://kertilampa.local");
}

void loop()
{
  server.handleClient();
  reconnectWifiIfNeeded();

  if (WiFi.status() == WL_CONNECTED && !ntpConfigured)
    startNtp();
  else if (WiFi.status() != WL_CONNECTED)
    ntpConfigured = false;

  checkDailySchedule();
  checkOtaSchedule();
  updateLightOutput();
  delay(2);
}
