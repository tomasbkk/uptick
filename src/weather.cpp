#include "uptick.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Weather from Open-Meteo: no key, no account, and every city in a single
// request - latitude and longitude take comma-separated lists, and the answer
// is then a JSON array with one object per city in the same order (a single
// city comes back as a bare object). timezone=auto gives each city its own
// local days, so a forecast for Bangkok starts on Bangkok's today.
//
// About 650 bytes per city for five days, so it is parsed with ArduinoJson
// behind a filter rather than streamed by hand like the candle responses.

CityWeather       g_weather[UPTICK_MAX_CITIES];
SemaphoreHandle_t g_weatherMutex = nullptr;

// Open-Meteo updates current conditions every 15 minutes, so polling faster
// would only fetch the same numbers again.
static const uint32_t WX_EVERY_MS = 15UL * 60UL * 1000UL;
static const uint32_t WX_RETRY_MS = 60UL * 1000UL;

static uint32_t s_lastTryMs = 0;
static bool     s_lastOk    = false;
static String   s_fetchedSig;          // units and coordinates of the data in g_weather

void weatherBegin()
{
  g_weatherMutex = xSemaphoreCreateMutex();
}

// Sakamoto's method. 0 = Sunday.
static uint8_t dayOfWeek(int y, int m, int d)
{
  static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 3) y -= 1;
  return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

static int8_t toDegrees(JsonVariantConst v)
{
  const long r = lroundf(v.as<float>());
  return (int8_t)constrain(r, -128L, 127L);
}

static void parseCity(JsonObjectConst c, CityWeather& w)
{
  JsonObjectConst now = c["current"];
  JsonObjectConst day = c["daily"];
  if (now.isNull() || day.isNull() || now["temperature_2m"].isNull()) return;

  w.temp      = now["temperature_2m"].as<float>();
  w.code      = now["weather_code"] | 0;
  w.isDay     = (now["is_day"] | 1) != 0;
  w.utcOffset = c["utc_offset_seconds"] | 0;

  JsonArrayConst dates = day["time"];
  JsonArrayConst codes = day["weather_code"];
  JsonArrayConst highs = day["temperature_2m_max"];
  JsonArrayConst lows  = day["temperature_2m_min"];

  uint8_t n = 0;
  for (size_t k = 0; k < dates.size() && n < UPTICK_WX_DAYS; k++)
  {
    int y, m, d;
    if (sscanf(dates[k] | "", "%d-%d-%d", &y, &m, &d) != 3) continue;
    if (highs[k].isNull() || lows[k].isNull()) continue;

    WeatherDay& wd = w.day[n++];
    wd.dow  = dayOfWeek(y, m, d);
    wd.code = codes[k] | 0;
    wd.hi   = toDegrees(highs[k]);
    wd.lo   = toDegrees(lows[k]);
  }
  w.days      = n;
  w.valid     = true;
  w.fetchedMs = millis();
}

static bool fetchAll(const String& units, uint8_t n, const float* lat, const float* lon)
{
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  for (uint8_t i = 0; i < n; i++) url += String(lat[i], 4) + (i + 1 < n ? "," : "");
  url += "&longitude=";
  for (uint8_t i = 0; i < n; i++) url += String(lon[i], 4) + (i + 1 < n ? "," : "");
  url += "&current=temperature_2m,weather_code,is_day"
         "&daily=weather_code,temperature_2m_max,temperature_2m_min"
         "&timezone=auto&forecast_days=" + String(UPTICK_WX_DAYS);
  if (units == "f") url += "&temperature_unit=fahrenheit";

  WiFiClientSecure client;
  HTTPClient       https;
  marketPrepareClient(client, https);

  if (!https.begin(client, url))
  {
    Serial.println("[wx] begin failed");
    return false;
  }

  const int code = https.GET();
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[wx] http %d\n", code);
    https.end();
    return false;
  }

  // One filter describes a city; for several cities it goes in an array,
  // which ArduinoJson applies to every element.
  JsonDocument filter;
  JsonObject   f = (n > 1) ? filter[0].to<JsonObject>() : filter.to<JsonObject>();
  f["utc_offset_seconds"]           = true;
  f["current"]["temperature_2m"]    = true;
  f["current"]["weather_code"]      = true;
  f["current"]["is_day"]            = true;
  f["daily"]["time"]                = true;
  f["daily"]["weather_code"]        = true;
  f["daily"]["temperature_2m_max"]  = true;
  f["daily"]["temperature_2m_min"]  = true;

  JsonDocument doc;
  const DeserializationError e =
      deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (e)
  {
    Serial.printf("[wx] parse: %s\n", e.c_str());
    return false;
  }

  CityWeather out[UPTICK_MAX_CITIES];
  uint8_t     ok = 0;
  for (uint8_t i = 0; i < n; i++)
  {
    JsonObjectConst c = (n > 1) ? doc[i].as<JsonObjectConst>() : doc.as<JsonObjectConst>();
    parseCity(c, out[i]);
    if (out[i].valid) ok++;
  }
  if (!ok)
  {
    Serial.println("[wx] no usable city in the answer");
    return false;
  }

  {
    WeatherLock lk;
    for (uint8_t i = 0; i < UPTICK_MAX_CITIES; i++) g_weather[i] = (i < n) ? out[i] : CityWeather();
  }

  Serial.printf("[wx] %u of %u cities, first %.1f code %u, %u days\n",
                ok, n, out[0].temp, out[0].code, out[0].days);
  return true;
}

void weatherPoll()
{
  if (g_net.apMode || WiFi.status() != WL_CONNECTED) return;

  // What to fetch, re-read only when the config moves. The signature is the
  // units and every coordinate, in order: when it differs from the data on
  // hand, that data belongs to a different list and is dropped at once rather
  // than shown under the wrong names.
  static uint32_t seenRev = 0xFFFFFFFF;
  static bool     wanted = false;
  static String   units, sig;
  static uint8_t  n = 0;
  static float    lat[UPTICK_MAX_CITIES], lon[UPTICK_MAX_CITIES];

  if (seenRev != g_cfgRev)
  {
    CfgLock lk;
    seenRev = g_cfgRev;
    // The clock needs the weather fetch too: it takes its city's UTC offset
    // from it, which follows daylight saving with no time zone tables.
    wanted  = g_cfg.showWeather || g_cfg.showClock;
    units   = g_cfg.weatherUnits;
    n       = g_cfg.cityCount;
    sig     = units;
    for (uint8_t i = 0; i < n; i++)
    {
      lat[i] = g_cfg.cities[i].lat;
      lon[i] = g_cfg.cities[i].lon;
      sig   += ";" + String(lat[i], 4) + "," + String(lon[i], 4);
    }
  }

  const bool changed = (sig != s_fetchedSig);
  if (changed && s_fetchedSig.length())
  {
    WeatherLock lk;
    for (auto& w : g_weather) w = CityWeather();
    s_fetchedSig = "";
  }

  if (!wanted || n == 0) return;

  const uint32_t now  = millis();
  const uint32_t wait = s_lastOk ? WX_EVERY_MS : WX_RETRY_MS;
  if (s_lastTryMs && now - s_lastTryMs < (changed && s_lastOk ? 0 : wait)) return;

  s_lastTryMs = now;
  s_lastOk    = fetchAll(units, n, lat, lon);
  if (s_lastOk) s_fetchedSig = sig;
}
