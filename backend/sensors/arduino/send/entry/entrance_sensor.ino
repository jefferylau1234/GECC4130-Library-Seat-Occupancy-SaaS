/*
  Entrance sensor with HC-SR04
  - Trigger 10us pulse, measure echo
  - Detect pass -> record timestamp + signal bool
  - Attempt HTTPS/HTTP POST of buffered events
  - If upload fails: keep in circular buffer and retry periodically

  JSON for each record:
  { "signal": true, "time": 1628561234.123456 }

  Target: ESP32 core (uses esp_timer_get_time())
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_sleep.h"

// ---------------------------- Configuration ----------------------------

// Server
// Use http://... or https://...
const char* SERVER_URL = "https://cc-library-dashboard-ecegewg6bqfracfd.austriaeast-01.azurewebsites.net/api/sensor/occupancy";

const char* SENSOR_KEY = "ABCDEFG12345";
// If using HTTPS and want verification you should use WiFiClientSecure and setCACert.

// Pins (change if needed)
const int PIN_TRIG = 17;
const int PIN_ECHO = 16;
const int PIN_LED = 2;

// Sensor thresholds
constexpr float MAX_DISTANCE_CM = 92.0f;                              // HC-SR04 max
constexpr float DETECT_RATIO = 0.6f;                                  // detection threshold as ratio of max (you used detect_bounder * max_distance)
constexpr float DETECT_DISTANCE_CM = DETECT_RATIO * MAX_DISTANCE_CM;  // computed detection distance

// Timing and buffer
const unsigned long MIN_TIME_DIFF_US = 300000UL;       // min separation between valid records in microseconds (1 s)
const unsigned long SCAN_DELAY_MS = 200;               // loop delay between scans
const unsigned long UPLOAD_INTERVAL_MS = 10 * 1000UL;  // try sending buffer every 10s
const int BUFFER_CAPACITY = 75;
const int UPLOAD_THRESHOLD = BUFFER_CAPACITY - 20;  // upload when buffer >= UPLOAD_THRESHOLD
const int MAX_RETRIES = 1;
const unsigned long RETRY_BACKOFF_MS = 500;

// Signal configuration (user wants "const bool // I need to config it at begin")
const bool SIGNAL_VALUE = 1;  // set once at top of sketch if the recorded signal should be true or false

// Logging
#define LOG_ENABLED 1

// ---------------------------- Record & Circular Buffer ----------------------------

struct Record {
  bool signal;       // the configured signal value
  float time_s;      // wall-clock time in seconds (for uploads, keeps fractional part)
  uint32_t time_ms;  // monotonic time (millis()) used for spacing/de-duplication
};

class CircularBuffer {
public:
  CircularBuffer()
    : head(0), count(0) {}

  bool add(const Record& r) {
    if (count == 0) {
      // Always add first record
      buffer[head] = r;
      head = (head + 1) % BUFFER_CAPACITY;
      if (count < BUFFER_CAPACITY) ++count;
      return true;
    }

    // index of last stored record
    int lastIdx = (head + BUFFER_CAPACITY - 1) % BUFFER_CAPACITY;
    const Record& last = buffer[lastIdx];

    // wrap-safe difference using unsigned arithmetic
    uint32_t last_ms = last.time_ms;
    uint32_t cur_ms = r.time_ms;
    uint32_t diff_ms = cur_ms - last_ms;  // unsigned wrap-around works correctly

    if (diff_ms < MIN_TIME_DIFF_US) {
      // too close -> reject
      return false;
    }

    // Add to buffer (overwrite oldest when full)
    buffer[head] = r;
    head = (head + 1) % BUFFER_CAPACITY;
    if (count < BUFFER_CAPACITY) ++count;
    return true;
  }
  void clear() {
    head = 0;
    count = 0;
  }

  int size() const {
    return count;
  }

  void pushOverwrite(const Record& rec) {
    buffer[head] = rec;
    head = (head + 1) % BUFFER_CAPACITY;
    if (count < BUFFER_CAPACITY) ++count;
  }

  bool popNewest() {
    if (count == 0) return false;  // nothing to pop

    // newest element is at head-1 (wrap around)
    head = (head + BUFFER_CAPACITY - 1) % BUFFER_CAPACITY;
    // simply forget the item by reducing count; optionally clear slot if desired
    --count;
    return true;
  }


  // get element by index 0..(size()-1) in chronological order (oldest first)
  Record getByIndex(int idx) const {
    // convert idx in [0, count-1] to real array index
    int start = (head + BUFFER_CAPACITY - count) % BUFFER_CAPACITY;
    int real = (start + idx) % BUFFER_CAPACITY;
    return buffer[real];
  }


private:
  Record buffer[BUFFER_CAPACITY];
  int head;   // next write position
  int count;  // valid stored records
  unsigned long MIN_TIME_DIFF_US = 300000UL;
};

// ---------------------------- normal global variables ----------------------------
unsigned long lastUploadAttempt = 0;
int MODE = 1;               // mode of buffer update if singal = 1 then mode = 1
CircularBuffer dataBuffer;  // init the buffer
bool activeWindow = false;  // set in setup() if we should run loop()


// ---------------------------- Utilities ----------------------------


static bool formatEpochToYMD_HMS(double epochSeconds, char* outBuf, size_t outBufSize) {
  if (outBufSize < 20) return false;  // needs 19 chars + NUL

  time_t sec = (time_t)floor(epochSeconds);
  struct tm tm_now;
  if (localtime_r(&sec, &tm_now) == nullptr) return false;

  int n = snprintf(outBuf, outBufSize, "%04d-%02d-%02d_%02d:%02d:%02d",
                   tm_now.tm_year + 1900,
                   tm_now.tm_mon + 1,
                   tm_now.tm_mday,
                   tm_now.tm_hour,
                   tm_now.tm_min,
                   tm_now.tm_sec);
  return (n > 0 && n < (int)outBufSize);
}

// Serialize CircularBuffer to JSON array of objects:
// [{"signal":true,"recorded_at":"YYYY-MM-DD_HH:MM:SS"}, ...]
String serializeBufferToJson(const CircularBuffer& buf) {
  String json = "[";
  int count = buf.size();
  for (int i = 0; i < count; ++i) {
    Record r = buf.getByIndex(i);

    char timestr[24];  // 19 chars + NUL, 24 is safe
    bool ok = formatEpochToYMD_HMS(r.time_s, timestr, sizeof(timestr));
    if (!ok) {
      // fallback to zero-time string if formatting fails
      strncpy(timestr, "1970-01-01_00:00:00", sizeof(timestr));
      timestr[sizeof(timestr) - 1] = '\0';
    }

    const char* sigstr = r.signal ? "true" : "false";
    // build the object: {"signal":true,"recorded_at":"YYYY-MM-DD_HH:MM:SS"}
    char tmp[128];
    int n = snprintf(tmp, sizeof(tmp),
                     "{\"signal\":%s,\"recorded_at\":\"%s\"}",
                     sigstr, timestr);
    if (n > 0 && n < (int)sizeof(tmp)) {
      json += tmp;
    } else {
      // fallback object on formatting error
      json += "{\"signal\":false,\"recorded_at\":\"1970-01-01_00:00:00\"}";
    }

    if (i < count - 1) json += ",";
  }
  json += "]";
  return json;
}


// mode: -1 => read actual Record values (r.value), 0 => all zeros, 1 => all ones
/*
String serializeBufferToJson(const CircularBuffer& buf, int mode = -1) {
  int n = buf.size();
  if (n == 0) return String("{\"signal\":[]}");

  String json;
  json.reserve(12 + n * 2);  // rough reservation
  json += "{\"signal\":[";

  for (int i = 0; i < n; ++i) {
    int v;
    if (mode == -1) {
      Record r = buf.getByIndex(i);  // oldest-first
      v = r.signal ? 1 : 0;          // adapt if Record::value is not bool/int
    } else {
      v = (mode == 0) ? 0 : 1;
    }

    json += String(v);
    if (i + 1 < n) json += ',';
  }

  json += "]}";
  return json;
}
*/
bool uploadData(const String& payload) {
  if (payload.length() == 0) return true;  // nothing to do

  // Choose appropriate client: plain HTTPClient uses underlying client automatically
  HTTPClient http;
  bool success = false;

  for (int attempt = 0; attempt < MAX_RETRIES && !success; ++attempt) {
    if (LOG_ENABLED) {
      Serial.printf("Uploading payload (attempt %d)... length=%u\n", attempt + 1, (unsigned)payload.length());
      Serial.println(payload);
    }

    if (!http.begin(SERVER_URL)) {  // plain begin (if you need secure client, replace with WiFiClientSecure)
      if (LOG_ENABLED) Serial.println("HTTP begin failed");
      break;
    }
    http.addHeader("Content-Type", "application/json");
    // add extra headers if you use x-sensor-key etc.
    http.addHeader("x-sensor-key", SENSOR_KEY);

    int code = http.POST((uint8_t*)payload.c_str(), payload.length());

    if (code > 0) {
      if (LOG_ENABLED) Serial.printf("HTTP status: %d\n", code);
      String resp = http.getString();
      if (LOG_ENABLED) Serial.println(resp);

      if (code >= 200 && code < 300) {
        success = true;
      } else {
        if (LOG_ENABLED) Serial.printf("Server responded non-2xx: %d\n", code);
      }
    } else {
      if (LOG_ENABLED) Serial.printf("HTTP POST failed, error: %s\n", http.errorToString(code).c_str());
    }

    http.end();

    if (!success && attempt < (MAX_RETRIES - 1)) delay(RETRY_BACKOFF_MS);
  }

  return success;
}

// Upload all buffered data (attempt). On success clears buffer.
bool sendBufferedData() {
  if (dataBuffer.size() == 0) return true;

  String json = serializeBufferToJson(dataBuffer);
  bool ok = uploadData(json);
  if (ok) {
    dataBuffer.clear();
    if (LOG_ENABLED) Serial.println("Buffered data uploaded and cleared.");
  } else {
    if (LOG_ENABLED) Serial.println("Buffered upload failed, will retry later.");
  }
  return ok;
}

// Compute precise current time in seconds (float)
// Uses system time() for seconds and esp_timer_get_time() for microseconds fraction.
float currentTimeSeconds() {
  time_t sec = time(nullptr);
  // esp_timer_get_time returns microseconds since boot
  int64_t us_since_boot = esp_timer_get_time() % 1000000LL;
  // Combine: sec + fractional part from esp_timer_get_time
  float t = (float)sec + (float)(us_since_boot) / 1000000.0f;
  return t;
}
//
/*
bool uploadSingleRecord(const Record& r) {
  char tmp[96];
  const char* sigstr = r.signal ? "true" : "false";
  int n = snprintf(tmp, sizeof(tmp), "{\"signal\":%s,\"time\":%.6f}", sigstr, r.time_s);
  if (n <= 0) return false;
  String payload = String(tmp);
  return uploadData(payload);  // uses the existing uploadData() retry logic
}
*/

// Example Record for reference
// struct Record { bool signal; float time_s; };

#include <time.h>

// Example Record:
// struct Record { bool signal; double time_s; };

bool uploadSingleRecord(const Record& r) {
  // Convert to integer seconds (drop fractional part)
  time_t sec = (time_t)floor(r.time_s);
  // convert to broken-down local time (use gmtime_r for UTC)
  struct tm tm_now;
  if (localtime_r(&sec, &tm_now) == nullptr) return false;

  // format "YYYY-MM-DD_HH:MM:SS" -> 19 chars + NUL
  char timestr[24];
  int tn = snprintf(timestr, sizeof(timestr), "%04d-%02d-%02d_%02d:%02d:%02d",
                    tm_now.tm_year + 1900,
                    tm_now.tm_mon + 1,
                    tm_now.tm_mday,
                    tm_now.tm_hour,
                    tm_now.tm_min,
                    tm_now.tm_sec);
  if (tn <= 0 || tn >= (int)sizeof(timestr)) return false;

  // create JSON: {"signal":true,"time":"YYYY-MM-DD_HH:MM:SS"}
  char tmp[128];
  const char* sigstr = r.signal ? "true" : "false";
  int n = snprintf(tmp, sizeof(tmp), "{\"signal\":%s,\"time\":\"%s\"}", sigstr, timestr);
  if (n <= 0 || n >= (int)sizeof(tmp)) return false;

  String payload = String(tmp);
  return uploadData(payload);
}
/*
bool uploadSingleRecord(const Record& r) {
  // Convert boolean signal to integer 0 or 1
  int sigInt = r.signal ? 1 : 0;

  // Build JSON body: {"signal":0} or {"signal":1}
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "{\"signal\":%d}", sigInt);
  if (n <= 0 || n >= (int)sizeof(tmp)) return false;  // snprintf error or truncated

  String payload = String(tmp);
  return uploadData(payload);  // reuse existing uploadData() (which should set Content-Type header)
}
*/

// ---------- time window config ----------
const int START_HOUR = 8;  // active start (08:00)
const int END_HOUR = 22;   // active end   (22:00)
const uint64_t uS_TO_S_FACTOR = 1000000ULL;



time_t nowTime() {
  return time(nullptr);
}

uint64_t secondsUntil(int targetHour, int targetMin = 0, int targetSec = 0) {
  time_t now = nowTime();
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  struct tm tm_target = tm_now;
  tm_target.tm_hour = targetHour;
  tm_target.tm_min = targetMin;
  tm_target.tm_sec = targetSec;
  time_t t_target = mktime(&tm_target);
  if (t_target <= now) t_target += 24 * 3600;
  return (uint64_t)(t_target - now);
}

// Required for WPA2-Enterprise (prefers esp_eap_client.h if available)
#if __has_include("esp_eap_client.h")
#include "esp_eap_client.h"
#else
#include "esp_wpa2.h"
#endif
#include "esp_wifi.h"

// ======= enterprise credentials =======
const char* WIFI_SSID = "eduroam";
const char* EAP_IDENTITY = "";  // optional anonymous id (can be same as username or empty)
const char* EAP_USERNAME = "s1155212946@cuhk.edu.hk";
const char* EAP_PASSWORD = "CzRvc4QqbA";

// ======= Testing WIFI =======
const char* SSID = "POCOF7";
const char* WIFI_PASSWORD = "e6bq2qvbuq";
const bool WIFI_FLAG = 0;

// If you have the RADIUS server CA cert (PEM) paste it here and uncomment the esp_wpa2_set_ca_cert() call:
// const char ca_cert_pem[] = "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n";
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // init pins used by loop
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  digitalWrite(PIN_LED, LOW);

  // --------- WPA2-Enterprise connection for time sync ----------
  Serial.println();
  Serial.println("Starting WPA2-Enterprise WiFi for time sync...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);

  // Set EAP identity/username/password using whichever API is available
  if (WIFI_FLAG) {
#if __has_include("esp_eap_client.h")
    // New API (recommended)
    if (strlen(EAP_IDENTITY) > 0) {
      esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
    } else {
      esp_eap_client_clear_identity();
    }
    esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
    esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));

    // Enable enterprise
    esp_wifi_sta_enterprise_enable();
#else
    // Fallback to old API on very old cores
    if (strlen(EAP_IDENTITY) > 0) {
      esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
    }
    esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
#endif
    // Start connecting (Enterprise uses username/password, not pre-shared key)
    WiFi.begin(WIFI_SSID);
  }else {WiFi.begin(SSID, WIFI_PASSWORD);}


  unsigned long tstart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - tstart < 7000) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // set timezone offset if you need local time (here +8h). Use your correct offset or TZ string.
    configTime(8 * 3600, 0, "pool.ntp.org");
    Serial.println("WiFi available for time sync");

    unsigned long w = millis();
    while (time(nullptr) < 1609459200UL && millis() - w < 5000) delay(200);

    // finished; disconnect WiFi and cleanup enterprise state
    /*
    WiFi.disconnect(true);
    #if __has_include("esp_eap_client.h")
      esp_wifi_sta_enterprise_disable();
      esp_eap_client_clear_identity();
      esp_eap_client_clear_username();
      esp_eap_client_clear_password();
    #else
      esp_wifi_sta_wpa2_ent_disable();
    #endif
    WiFi.mode(WIFI_OFF);
    */
  } else {
    Serial.println("WiFi not available for time sync (enterprise auth failed / timeout)");

// cleanup on failure
#if __has_include("esp_eap_client.h")
    esp_wifi_sta_enterprise_disable();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
#else
    esp_wifi_sta_wpa2_ent_disable();
#endif
    WiFi.mode(WIFI_OFF);
  }

  // ... rest of your original setup follows unchanged ...
  time_t now = nowTime();
  if (now < 1609459200UL) {
    Serial.println("No reliable time. Sleeping 1 hour before retry.");
    esp_sleep_enable_timer_wakeup(3600 * uS_TO_S_FACTOR);  // 1 hour
    esp_deep_sleep_start();
  }

  struct tm tm_now;
  localtime_r(&now, &tm_now);
  int hour = tm_now.tm_hour;
  Serial.printf("Boot at %02d:%02d:%02d\n", hour, tm_now.tm_min, tm_now.tm_sec);

  if (!(hour >= START_HOUR && hour < END_HOUR)) {
    uint64_t secs = secondsUntil(START_HOUR, 0, 0);
    Serial.printf("Outside active window. Sleeping %llu seconds until %02d:00.\n", secs, START_HOUR);
    esp_sleep_enable_timer_wakeup(secs * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }

  activeWindow = true;
  lastUploadAttempt = millis() - UPLOAD_INTERVAL_MS;
}

// ---------------------------- Loop ----------------------------
void loop() {
  // If activeWindow somehow false, bail (shouldn't happen because we slept if outside)
  if (!activeWindow) {
    // safety: sleep a bit and restart
    esp_sleep_enable_timer_wakeup(1 * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }

  // --- your existing HC-SR04 read and detection logic ---
  // 1) Trigger HC-SR04 for 10us
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(30);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(30);  // required 10us
  digitalWrite(PIN_TRIG, LOW);

  unsigned long timeout_us = (unsigned long)(MAX_DISTANCE_CM * 58.0f) + 120000UL;  // add margin with recommended margin 60ms
  unsigned long duration_us = pulseIn(PIN_ECHO, HIGH, timeout_us);

  if (duration_us == 0) {
#if LOG_ENABLED
    Serial.println("No echo (timeout).");
#endif
  } else {
    float distance_cm = (float)duration_us / 58.0f;
    Serial.println("Current distance");
    Serial.println(distance_cm);
    // detection & buffering logic unchanged
    if (distance_cm <= DETECT_DISTANCE_CM) {
      float ts = currentTimeSeconds();
      uint32_t ts_ms = millis();

      Record rec;
      rec.signal = SIGNAL_VALUE;
      rec.time_s = ts;
      rec.time_ms = ts_ms;

      bool acceptedBySpacing = dataBuffer.add(rec);

      if (acceptedBySpacing) {
#if LOG_ENABLED
        Serial.println("Valid event. Attempt single upload...");
#endif
        // Ensure WiFi connection only when needed
        if (WIFI_FLAG) {
          WiFi.begin(WIFI_SSID);
        } else {
          WiFi.begin(SSID, WIFI_PASSWORD);
        }
        unsigned long s2 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - s2 < 7000) delay(50);

        if (WiFi.status() == WL_CONNECTED) {
          if (uploadSingleRecord(rec)) {
#if LOG_ENABLED
            Serial.println("Single-record upload successful. Removing from buffer.");
#endif
            dataBuffer.popNewest();
          } else {
#if LOG_ENABLED
            Serial.println("Single-record upload failed. Keeping in buffer.");
#endif
          }
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        } else {
#if LOG_ENABLED
          Serial.println("WiFi connect failed for single upload; keeping buffer.");
#endif
        }
      } else {
#if LOG_ENABLED
        Serial.println("Event too close to previous. Ignoring.");
#endif
      }

      if (WiFi.status() == WL_CONNECTED && dataBuffer.size() >= UPLOAD_THRESHOLD) {
        if (sendBufferedData()) {
          digitalWrite(PIN_LED, HIGH);
          delay(80);
          digitalWrite(PIN_LED, LOW);
        }
      }
    }
  }

  // --- Periodic buffered upload attempt (unchanged) ---
  unsigned long now_ms = millis();
  if (WiFi.status() == WL_CONNECTED && (now_ms - lastUploadAttempt >= UPLOAD_INTERVAL_MS)) {
    lastUploadAttempt = now_ms;
    if (dataBuffer.size() > 0) {
      if (sendBufferedData()) {
        digitalWrite(PIN_LED, HIGH);
        delay(80);
        digitalWrite(PIN_LED, LOW);
      }
    }
  }

  // After each loop iteration, check whether the active window has ended.
  time_t now = nowTime();
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  int hourNow = tm_now.tm_hour;
  if (!(hourNow >= START_HOUR && hourNow < END_HOUR)) {
    // Active window ended -> compute seconds until next start and go to deep sleep
    uint64_t secs = secondsUntil(START_HOUR, 0, 0);
    Serial.printf("Active window ended at %02d:%02d:%02d. Sleeping %llu seconds until next start.\n",
                  tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, secs);
    esp_sleep_enable_timer_wakeup(secs * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
    // never returns
  }

  // keep at least the recommended delay between HC-SR04 scans
  delay(SCAN_DELAY_MS);
}