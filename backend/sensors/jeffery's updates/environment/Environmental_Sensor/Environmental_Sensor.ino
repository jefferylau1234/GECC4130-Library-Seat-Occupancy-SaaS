#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <driver/i2s.h>
#include <math.h>
#include "esp_wifi.h"

#if __has_include("esp_eap_client.h")
  #include "esp_eap_client.h"
  #define USE_NEW_EAP_API 1
#else
  #include "esp_wpa2.h"
  #define USE_NEW_EAP_API 0
#endif

// ======================== EDIT ONLY THIS SECTION ========================
// Copy these three values from the working eduroam section of your gate code.



const char* EAP_WIFI_SSID = "eduroam";
const char* EAP_USERNAME = "794519kl@eur.nl";
const char* EAP_PASSWORD = "Strike-Buffet-All-out-Trait";

const char* ZONE = "2F_study7";



// eduroam
// s1155214617@link.cuhk.edu.hk       a135246357      4
// s1155212946@cuhk.edu.hk            CzRvc4QqbA      
// s1155210182@link.cuhk.edu.hk       Yan24827513     
// s1155214508@link.cuhk.edu.hk       UiSUFNJFe5NCLxL 
// 794519kl@eur.nl                    Strike-Buffet-All-out-Trait




// CUHK1x
//
//
//
//




/*
zoneIds = [
  // Second Floor
  "2F_hub1",     @
  "2F_study2",   @
  "2F_study3",   @
  "2F_study4",   @
  "2F_study6",   @
  "2F_study5",   @
  "2F_study7",
  "2F_study8",
  "2F_study9",
  "2F_pc10",

  // First Floor
  "1F_study1",  @
  "1F_study2",
  "1F_study3",
  "1F_study4",  @
  "1F_study6",
  "1F_study5",
  "1F_study10",
  "1F_study7",
  "1F_hub8",    @
  "1F_study9",
  "1F_study11",
  "1F_study12",
  "1F_pc13",

  // Ground Floor
  "GF_study5",
  "GF_study9",
  "GF_study2",
  "GF_study6",
  "GF_study7",
  "GF_pc8",
  "GF_pc3",      @
  "GF_study4",
  "GF_study1",  @
];
*/


// ========================================================================
const char* EAP_IDENTITY = "";  

const char* API_BASE = "https://cc-library-dashboard-ecegewg6bqfracfd.austriaeast-01.azurewebsites.net";
const char* SENSOR_KEY = "ABCDEFG12345";


// Set DHT_TYPE to DHT11 or DHT22 according to the label on your sensor.
const int DHT_PIN = 4;
const int DHT_TYPE = DHT11;

// INMP441: SD -> GPIO 9, BCK/SCK -> GPIO 10, WS/LRCL -> GPIO 11.
const int I2S_SD = 9;
const int I2S_BCK = 10;
const int I2S_WS = 11;
const uint32_t I2S_SAMPLE_RATE = 16000;

const uint32_t NOISE_MEASURE_SECONDS = 5;
const unsigned long UPLOAD_INTERVAL_MS = 30UL * 60UL * 1000UL;                  // pass data every 30 minutes
const unsigned long WIFI_TIMEOUT_MS = 20000UL;
const int HTTP_TIMEOUT_MS = 8000;
const int MAX_HTTP_RETRIES = 3;

const float INMP441_SENSITIVITY_DBFS_AT_94_SPL = 0.0f;
const float INMP441_REFERENCE_SPL = 94.0f;
// ========================================================================

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClientSecure wifiClient;
bool micReady = false;
unsigned long lastUploadMs = 0;

struct NoiseResult {
  float rms;
  float dbfs;
  float dbSpl;
  float peakDbfs;
  uint32_t samples;
};

void configureEduroam() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_STA);
  delay(200);

#if USE_NEW_EAP_API
  if (strlen(EAP_IDENTITY) > 0) {
    esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  } else {
    esp_eap_client_clear_identity();
  }
  esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  esp_wifi_sta_enterprise_enable();
#else
  if (strlen(EAP_IDENTITY) > 0) {
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  }
  esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
  esp_wifi_sta_wpa2_ent_enable();
#endif
}

bool connectEduroam() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("Connecting to WPA2-Enterprise WiFi: eduroam");
  WiFi.begin(EAP_WIFI_SSID);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("eduroam connection failed or timed out. WiFi status=%d\n", WiFi.status());
    return false;
  }

  Serial.print("eduroam connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool initMicrophone() {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = I2S_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_I2S_MSB;
  config.intr_alloc_flags = 0;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_BCK;
  pins.ws_io_num = I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_SD;

  esp_err_t result = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    Serial.printf("INMP441 I2S driver install failed: %d\n", result);
    return false;
  }
  result = i2s_set_pin(I2S_NUM_0, &pins);
  if (result != ESP_OK) {
    Serial.printf("INMP441 I2S pin setup failed: %d\n", result);
    return false;
  }
  i2s_set_sample_rates(I2S_NUM_0, I2S_SAMPLE_RATE);
  return true;
}

NoiseResult measureNoise(uint32_t seconds) {
  NoiseResult result = {0.0f, -INFINITY, -INFINITY, -INFINITY, 0};
  if (!micReady) return result;

  static int32_t sampleBuffer[512];
  uint64_t sumSquares = 0;
  uint32_t totalSamples = 0;
  int16_t peak = 0;
  unsigned long started = millis();

  while (millis() - started < seconds * 1000UL) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, sampleBuffer, sizeof(sampleBuffer), &bytesRead, pdMS_TO_TICKS(500));
    if (err != ESP_OK || bytesRead == 0) {
      delay(1);
      continue;
    }

    size_t count = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < count; ++i) {
      int16_t sample = (int16_t)(sampleBuffer[i] >> 16);
      int32_t value = sample;
      sumSquares += (uint64_t)(value * (int64_t)value);
      totalSamples++;
      int32_t absoluteSample = sample == INT16_MIN ? INT16_MAX : abs(sample);
      int32_t absolutePeak = peak == INT16_MIN ? INT16_MAX : abs(peak);
      if (absoluteSample > absolutePeak) peak = sample;
    }
    yield();
  }

  result.samples = totalSamples;
  if (totalSamples == 0) return result;

  double normalizedRms = sqrt((double)sumSquares / totalSamples) / INT16_MAX;
  result.rms = (float)normalizedRms;
  result.dbfs = normalizedRms > 0.0 ? 20.0f * log10f(result.rms) : -INFINITY;


  result.dbSpl = isfinite(result.dbfs)
    ? result.dbfs + INMP441_REFERENCE_SPL - INMP441_SENSITIVITY_DBFS_AT_94_SPL
    : -INFINITY;


  int32_t absolutePeak = peak == INT16_MIN ? INT16_MAX : abs(peak);
  float normalizedPeak = (float)absolutePeak / INT16_MAX;
  result.peakDbfs = normalizedPeak > 0.0f ? 20.0f * log10f(normalizedPeak) : -INFINITY;
  return result;
}

bool postReading(float temperatureC, float humidity, const NoiseResult& noise) {
  if (!connectEduroam()) return false;

  char url[256];
  snprintf(url, sizeof(url), "%s/api/sensor/environmental-data", API_BASE);
  char json[256];
  int n = snprintf(json, sizeof(json),
                   "{\"zone\":\"%s\",\"temperature_c\":%.2f,\"noise_db\":%.4f,\"humidity\":%.2f}",
                   ZONE, temperatureC, noise.dbSpl, humidity);
  if (n < 0 || (size_t)n >= sizeof(json)) return false;

  Serial.print("POST body: ");
  Serial.println(json);

  for (int attempt = 1; attempt <= MAX_HTTP_RETRIES; ++attempt) {
    HTTPClient https;
    if (!https.begin(wifiClient, url)) {
      Serial.println("HTTP begin failed.");
      delay(500 * attempt);
      continue;
    }
    https.setConnectTimeout(HTTP_TIMEOUT_MS);
    https.setTimeout(HTTP_TIMEOUT_MS);
    https.addHeader("Content-Type", "application/json");
    https.addHeader("x-sensor-key", SENSOR_KEY);

    int code = https.POST((uint8_t*)json, strlen(json));
    String response = code > 0 ? https.getString() : https.errorToString(code);
    https.end();
    Serial.printf("HTTP attempt %d/%d: code=%d, response=%s\n", attempt, MAX_HTTP_RETRIES, code, response.c_str());
    if (code >= 200 && code < 300) return true;
    delay(500 * attempt);
  }
  return false;
}

void measureAndUpload() {
  Serial.println("\n--- Environmental measurement ---");
  float temperatureC = dht.readTemperature();
  float humidity = dht.readHumidity();
  if (isnan(temperatureC) || isnan(humidity)) {
    Serial.println("DHT read failed. Check DHT type, VCC, GND, DATA, and pull-up resistor if required.");
    return;
  }
  Serial.printf("DHT: %.2f C, %.2f %% RH\n", temperatureC, humidity);

  Serial.printf("Measuring INMP441 noise for %lu seconds...\n", (unsigned long)NOISE_MEASURE_SECONDS);
  NoiseResult noise = measureNoise(NOISE_MEASURE_SECONDS);
  if (noise.samples == 0) {
    Serial.println("INMP441 produced no samples. Check 3V3, GND, SD, BCK, and WS wiring.");
    return;
  }
  Serial.printf("INMP441: samples=%lu RMS=%.6f dBFS=%.2f peak=%.2f\n",
                (unsigned long)noise.samples, noise.rms, noise.dbfs, noise.peakDbfs);

  Serial.println(postReading(temperatureC, humidity, noise) ? "POST OK." : "POST FAILED; it will retry on the next cycle.");
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("Standalone environmental sensor: direct eduroam -> API mode.");

  dht.begin();
  micReady = initMicrophone();
  Serial.println(micReady ? "INMP441 initialized." : "INMP441 initialization failed.");

  wifiClient.setInsecure(); // HTTPS encryption, without certificate validation.
  configureEduroam();
  lastUploadMs = millis() - UPLOAD_INTERVAL_MS; // first measurement happens immediately
}

void loop() {
  if (millis() - lastUploadMs >= UPLOAD_INTERVAL_MS) {
    lastUploadMs = millis();
    measureAndUpload();
  }
  delay(50); // The board stays awake continuously; there is no deep sleep.
}