#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>

// ----------------- PendingReading and ring buffer -----------------
struct PendingReading {
  char zone[32];
  float temperature_c;
  float humidity;
  float noise_rms;
  uint32_t when_ms;
};


// ----------------- CONFIG - Fill these -----------------
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";

const char *API_BASE = "https://cc-library-dashboard-ecegewg6bqfracfd.austriaeast-01.azurewebsites.net";
const char *SENSOR_KEY = "";       // x-sensor-key header value

// ----------------- CRC16-CCITT (poly 0x1021, init 0xFFFF) -----------------
static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; ++i) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  return crc;
}

// ----------------- Packed structs matching sender layout -----------------
struct __attribute__((packed)) DHTResult {
  uint8_t ok;
  float temperature;
  float humidity;
  uint32_t timestamp;
};

struct __attribute__((packed)) NoiseResult {
  float rms;
  float dbfs;
  float peak_dbfs;
  int16_t peak_sample;
  int16_t min_sample;
  int16_t max_sample;
  uint32_t samplesCount;
  uint32_t timestamp;
};

struct __attribute__((packed)) WirePayload {
  uint8_t version;
  uint16_t seq;
  int16_t sensor_id;
  DHTResult dht;
  NoiseResult noise;
  uint16_t crc16;
};

const size_t EXPECTED_PAYLOAD_SIZE = sizeof(WirePayload);

// ----------------- Helpers -----------------
void printHex(const uint8_t *data, int len) {
  for (int i = 0; i < len; ++i) {
    if (i && (i % 16) == 0) Serial.println();
    Serial.printf("%02X ", data[i]);
  }
  Serial.println();
}

static int8_t get_rssi_from_recv_info(const esp_now_recv_info_t *recv_info) {
#if defined(ESP_PLATFORM)
  if (recv_info && recv_info->rx_ctrl) {
    const wifi_pkt_rx_ctrl_t *ctrl = recv_info->rx_ctrl;
    if (ctrl) return ctrl->rssi;
  }
#endif
  return 0;
}

// ----------------- Mapping sensor_id -> zone -----------------
// sensor_zones.cpp
// Simple switch mapping from int id -> zone string (overviews removed).
#include <cstdint>

const char* sensorIdToZone(int16_t id) {
    switch (id) {
        // Ground floor (IDs 1–9)
        case 1:  return "GF_study5";
        case 2:  return "GF_study9";
        case 3:  return "GF_study2";
        case 4:  return "GF_study6";
        case 5:  return "GF_study7";
        case 6:  return "GF_pc8";
        case 7:  return "GF_pc3";
        case 8:  return "GF_study4";
        case 9:  return "GF_study1";

        // First floor (IDs 10–22)
        case 10: return "1F_study1";
        case 11: return "1F_study2";
        case 12: return "1F_study3";
        case 13: return "1F_study4";
        case 14: return "1F_study6";
        case 15: return "1F_study5";
        case 16: return "1F_study10";
        case 17: return "1F_study7";
        case 18: return "1F_hub8";
        case 19: return "1F_study9";
        case 20: return "1F_study11";
        case 21: return "1F_study12";
        case 22: return "1F_pc13";

        // Second floor (IDs 23–33)
        case 23: return "2F_hub1";
        case 24: return "2F_study2";
        case 25: return "2F_study3";
        case 26: return "2F_study4";
        case 27: return "2F_study6";
        case 28: return "2F_study5";
        case 29: return "2F_study7";
        case 30: return "2F_study8";
        case 31: return "2F_study9";
        case 32: return "2F_pc10";

        default: return "unknown";
    }
}

const int MAX_PENDING = 8;
static PendingReading pending[MAX_PENDING];
static int pending_head = 0;
static int pending_tail = 0;
static int pending_count = 0;

bool enqueuePending(const PendingReading &r) {
  if (pending_count >= MAX_PENDING) return false;
  pending[pending_tail] = r;
  pending_tail = (pending_tail + 1) % MAX_PENDING;
  pending_count++;
  return true;
}

bool dequeuePending(PendingReading &out) {
  if (pending_count == 0) return false;
  out = pending[pending_head];
  pending_head = (pending_head + 1) % MAX_PENDING;
  pending_count--;
  return true;
}

// ----------------- ESP-NOW callbacks -----------------
void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  char macStr[18] = { 0 };
  if (recv_info && recv_info->src_addr) {
    const uint8_t *mac_addr = recv_info->src_addr;
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  } else {
    strncpy(macStr, "unknown", sizeof(macStr));
  }

  int8_t rssi = get_rssi_from_recv_info(recv_info);
  Serial.printf("\n--- Received packet from %s (%d bytes) RSSI=%d dBm ---\n", macStr, len, rssi);

  if (len < 2) {
    Serial.println("Packet too short to contain CRC. Ignoring.");
    return;
  }

  uint16_t computed = crc16_ccitt(incomingData, (size_t)len - 2);
  uint16_t transmitted = (uint16_t)incomingData[len - 2] | ((uint16_t)incomingData[len - 1] << 8);
  Serial.printf("Computed CRC=0x%04X, Transmitted CRC=0x%04X\n", computed, transmitted);

  if (computed != transmitted) {
    Serial.println("CRC mismatch -> dropping packet. Raw bytes:");
    printHex(incomingData, len);
    return;
  }

  WirePayload pkt;
  memset(&pkt, 0, sizeof(pkt));
  size_t copyLen = min((size_t)len, sizeof(pkt));
  memcpy(&pkt, incomingData, copyLen);

  const char *zone = sensorIdToZone(pkt.sensor_id);

  PendingReading pr;
  memset(&pr, 0, sizeof(pr));
  strncpy(pr.zone, zone, sizeof(pr.zone) - 1);
  pr.temperature_c = pkt.dht.temperature;
  pr.humidity = pkt.dht.humidity;
  pr.noise_rms = pkt.noise.rms;
  pr.when_ms = pkt.dht.timestamp;

  if (!enqueuePending(pr)) {
    Serial.println("Pending queue full - dropping newest reading.");
  } else {
    Serial.printf("Enqueued: zone=%s temp=%.2f hum=%.2f noise=%.4f\n",
                  pr.zone, pr.temperature_c, pr.humidity, pr.noise_rms);
  }
}

void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  const uint8_t *mac = nullptr;
  if (tx_info) mac = tx_info->src_addr;

  char macStr[18] = "unknown";
  if (mac) {
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  const char *statusStr = (status == ESP_NOW_SEND_SUCCESS) ? "SUCCESS" : "FAIL";
  Serial.printf("onDataSent: peer=%s status=%s\n", macStr, statusStr);
}

// ----------------- HTTP POST helper -----------------
const int HTTP_TIMEOUT_MS = 8000;
const int MAX_HTTP_RETRIES = 3;

// Put this globally (one instance reused)
WiFiClientSecure wifiClient; // global, reused
HTTPClient httpsClient;      // global, reused

bool postReading(const PendingReading &r) {
  // Build URL and JSON into local char buffers
  char url[256];
  snprintf(url, sizeof(url), "%s/api/sensor/environmental-data", API_BASE);

  char json[256];
  int n = snprintf(json, sizeof(json),
           "{\"zone\":\"%s\",\"temperature_c\":%.2f,\"humidity\":%.4f,\"noise_db\":%.4f}",
           r.zone, r.temperature_c, r.humidity, r.noise_rms);
  if (n < 0 || (size_t)n >= sizeof(json)) return false;

  wifiClient.setInsecure(); // call once at startup is fine
  bool ok = false;
  for (int attempt = 1; attempt <= MAX_HTTP_RETRIES; ++attempt) {
    if (!httpsClient.begin(wifiClient, url)) {
      Serial.println("HTTP begin failed");
      delay(200);
      continue;
    }
    httpsClient.setConnectTimeout(HTTP_TIMEOUT_MS / 1000);
    httpsClient.addHeader("Content-Type", "application/json");
    httpsClient.addHeader("x-sensor-key", SENSOR_KEY);

    int code = httpsClient.POST((uint8_t*)json, strlen(json));
    if (code > 0) {
      String resp = httpsClient.getString();
      Serial.printf("HTTP response code: %d body: %s\n", code, resp.c_str());
      if (code >= 200 && code < 300) {
        ok = true;
        httpsClient.end();
        break;
      }
    } else {
      Serial.printf("HTTP POST failed, error: %s. Attempt %d/%d\n", httpsClient.errorToString(code).c_str(), attempt, MAX_HTTP_RETRIES);
    }
    httpsClient.end();
    delay(500 * attempt);
  }
  return ok;
}

// ----------------- setup / loop -----------------
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.printf("Expected WirePayload size = %zu bytes\n", EXPECTED_PAYLOAD_SIZE);

  WiFi.mode(WIFI_STA);
  delay(200);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while (true) delay(1000);
  }

  esp_err_t rcv = esp_now_register_recv_cb(onDataRecv);
  esp_err_t snd = esp_now_register_send_cb(onDataSent);
  if (rcv != ESP_OK) Serial.printf("esp_now_register_recv_cb failed: %d\n", (int)rcv);
  if (snd != ESP_OK) Serial.printf("esp_now_register_send_cb failed: %d\n", (int)snd);

  Serial.println("ESP-NOW receiver ready");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi SSID=%s ...\n", WIFI_SSID);

  String mac = WiFi.macAddress();
  Serial.print("ESP32 MAC Address: ");
  Serial.println(mac);

}

void loop() {
  static unsigned long lastConnCheck = 0;
  unsigned long now = millis();
  if (now - lastConnCheck > 5000) {
    lastConnCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi not connected, attempting reconnect...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    } else {
      Serial.print("WiFi connected. IP=");
      Serial.println(WiFi.localIP());
    }
  }

  if (pending_count > 0 && WiFi.status() == WL_CONNECTED) {
    PendingReading r;
    if (dequeuePending(r)) {
      bool ok = postReading(r);
      if (!ok) {
        Serial.println("POST failed; re-enqueueing (if space)");
        if (!enqueuePending(r)) {
          Serial.println("Re-enqueue failed - queue full; dropping reading.");
        }
        delay(200);
      } else {
        Serial.println("POST OK");
      }
    }
  } else {
    delay(200);
  }
}