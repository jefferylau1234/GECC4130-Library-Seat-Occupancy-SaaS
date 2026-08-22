#include <Arduino.h>
#include "Sensors.h"
#include <esp_now.h>
#include <WiFi.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

// wire payload that contains both results plus metadata and CRC
struct WirePayload {
  uint8_t version;    // protocol version
  uint16_t seq;       // packet sequence number
  int16_t sensor_id;  // small id for the node/sensor
  DHTResult dht;
  NoiseResult noise;
  uint16_t crc16;  // CRC-16 covering everything BEFORE this field
};
#pragma pack(pop)

// prints a buffer as hex bytes e.g. "01 0A FF 3C"
void print_hex_buf(const uint8_t *buf, size_t len) {
  for (size_t j = 0; j < len; ++j) {
    uint8_t b = buf[j];
    // print leading zero for single-nibble values
    if (b < 16) Serial.print('0');
    Serial.print(b, HEX);
    if (j + 1 < len) Serial.print(' ');
  }
  Serial.println();
}

// CRC-16/CCITT (poly 0x1021, init 0xFFFF)
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


// --- low-level LE writers/readers ---
static void write_u8(uint8_t *buf, size_t &i, uint8_t v) {
  buf[i++] = v;
}
static void write_i16_le(uint8_t *buf, size_t &i, int16_t v) {
  buf[i++] = (uint8_t)(v & 0xFF);
  buf[i++] = (uint8_t)((v >> 8) & 0xFF);
}
static void write_u16_le(uint8_t *buf, size_t &i, uint16_t v) {
  buf[i++] = (uint8_t)(v & 0xFF);
  buf[i++] = (uint8_t)((v >> 8) & 0xFF);
}
static void write_u32_le(uint8_t *buf, size_t &i, uint32_t v) {
  buf[i++] = (uint8_t)(v & 0xFF);
  buf[i++] = (uint8_t)((v >> 8) & 0xFF);
  buf[i++] = (uint8_t)((v >> 16) & 0xFF);
  buf[i++] = (uint8_t)((v >> 24) & 0xFF);
}
static void write_f32_le(uint8_t *buf, size_t &i, float v) {
  uint32_t iv;
  memcpy(&iv, &v, 4);
  write_u32_le(buf, i, iv);
}

static uint8_t read_u8(const uint8_t *buf, size_t &i) {
  return buf[i++];
}
static int16_t read_i16_le(const uint8_t *buf, size_t &i) {
  int16_t v = (int16_t)(buf[i] | (buf[i + 1] << 8));
  i += 2;
  return v;
}
static uint16_t read_u16_le(const uint8_t *buf, size_t &i) {
  uint16_t v = (uint16_t)(buf[i] | (buf[i + 1] << 8));
  i += 2;
  return v;
}
static uint32_t read_u32_le(const uint8_t *buf, size_t &i) {
  uint32_t v = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) | ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
  i += 4;
  return v;
}
static float read_f32_le(const uint8_t *buf, size_t &i) {
  uint32_t iv = read_u32_le(buf, i);
  float f;
  memcpy(&f, &iv, 4);
  return f;
}


// --- serialize/deserialize nested types (must match on both ends) ---
size_t serialize_DHTResult(uint8_t *buf, size_t idx, const DHTResult &d) {
  write_u8(buf, idx, d.ok ? 1 : 0);
  write_f32_le(buf, idx, d.temperature);
  write_f32_le(buf, idx, d.humidity);
  write_u32_le(buf, idx, d.timestamp);
  return idx;
}
size_t deserialize_DHTResult(const uint8_t *buf, size_t idx, DHTResult &d) {
  d.ok = read_u8(buf, idx) ? true : false;
  d.temperature = read_f32_le(buf, idx);
  d.humidity = read_f32_le(buf, idx);
  d.timestamp = read_u32_le(buf, idx);
  return idx;
}

size_t serialize_NoiseResult(uint8_t *buf, size_t idx, const NoiseResult &n) {
  write_f32_le(buf, idx, n.rms);
  write_f32_le(buf, idx, n.dbfs);
  write_f32_le(buf, idx, n.peak_dbfs);
  write_i16_le(buf, idx, n.peak_sample);
  write_i16_le(buf, idx, n.min_sample);
  write_i16_le(buf, idx, n.max_sample);
  write_u32_le(buf, idx, n.samplesCount);
  write_u32_le(buf, idx, n.timestamp);
  return idx;
}
size_t deserialize_NoiseResult(const uint8_t *buf, size_t idx, NoiseResult &n) {
  n.rms = read_f32_le(buf, idx);
  n.dbfs = read_f32_le(buf, idx);
  n.peak_dbfs = read_f32_le(buf, idx);
  n.peak_sample = read_i16_le(buf, idx);
  n.min_sample = read_i16_le(buf, idx);
  n.max_sample = read_i16_le(buf, idx);
  n.samplesCount = read_u32_le(buf, idx);
  n.timestamp = read_u32_le(buf, idx);
  return idx;
}

// --- Serialize entire WirePayload into buf. Returns total bytes written (including 2-byte CRC).
// Caller must ensure buf_capacity is large enough (46 bytes expected; take e.g. 64).
size_t serialize_WirePayload(const WirePayload &pkt, uint8_t *buf, size_t buf_capacity) {
  if (buf_capacity < 16) return 0;  // trivial guard
  size_t idx = 0;
  write_u8(buf, idx, pkt.version);
  write_u16_le(buf, idx, pkt.seq);
  write_i16_le(buf, idx, pkt.sensor_id);

  idx = serialize_DHTResult(buf, idx, pkt.dht);
  idx = serialize_NoiseResult(buf, idx, pkt.noise);

  // compute CRC over bytes [0..idx-1]
  uint16_t crc = crc16_ccitt(buf, idx);
  write_u16_le(buf, idx, crc);
  return idx;
}

// --- Deserialize and CRC-check. in_len is total bytes received (payload + CRC).
// Returns true on success and fills out_pkt. On failure returns false.
bool deserialize_WirePayload(const uint8_t *in_buf, size_t in_len, WirePayload &out_pkt) {
  if (in_len < 2) return false;
  size_t payload_len = in_len - 2;
  // extract CRC (LE)
  uint16_t tx_crc = (uint16_t)in_buf[payload_len] | ((uint16_t)in_buf[payload_len + 1] << 8);
  uint16_t calc = crc16_ccitt(in_buf, payload_len);
  if (calc != tx_crc) {
    // CRC mismatch
    return false;
  }
}
// ESP NOW Configuration
const unsigned long SEND_INTERVAL_MS = 5UL * 1000UL;                     // 30s
const uint8_t RECEIVER_MAC[6] = { 0x30, 0x30, 0xF9, 0x68, 0x2A, 0x48 };  // replace it by recevier mac in byte

volatile bool send_in_progress = false;
volatile esp_now_send_status_t last_send_status = ESP_NOW_SEND_SUCCESS;
uint16_t send_seq = 0;
unsigned long lastSendMs = 0;

// SENSOR Configuration
// Pins and types
const int DHT_PIN = 4;
const int DHT_TYPE = 11;  // 11 for DHT11, 22 for DHT22

const int I2S_WS = 11;
const int I2S_BCK = 10;
const int I2S_SD = 9;
const uint32_t I2S_SR = 16000;  // sample rate used by INMP441Sensor

// Measurement intervals (ms)
//const unsigned long DHT_INTERVAL_MS  = 5UL * 60UL * 1000UL; // secs * mins in secs * ms
//const unsigned long INMP_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 minutes

const unsigned long DHT_INTERVAL_MS = 5 * 1000UL;   // 30 seconds
const unsigned long INMP_INTERVAL_MS = 5 * 1000UL;  // 30 seconds

// INMP measurement duration (seconds) for measureNoise()
const uint32_t INMP_MEASURE_SECONDS = 5;  // how long to record during each measurement

// --- Objects ---
DHTSensor dhtSensor(DHT_PIN, DHT_TYPE);
INMP441Sensor micSensor(I2S_BCK, I2S_WS, I2S_SD, I2S_SR);

// Data store: keeps latest results from each sensor
//Remark struct TypeName { ... } instanceName;
//you're both defining the struct type and declaring a variable (instance) named instanceName of that type in the same statement.

unsigned long lastDhtMillis = 0;
unsigned long lastInmpMillis = 0;

void printDHTResult(const DHTResult &r) {
  if (!r.ok) {
    Serial.println("DHT: read failed");
    return;
  }
  Serial.print("DHT: temp=");
  Serial.print(r.temperature, 2);
  Serial.print(" C  hum=");
  Serial.print(r.humidity, 2);
  Serial.print(" %  ts=");
  Serial.println(r.timestamp);
}

void printNoiseResult(const NoiseResult &n) {
  Serial.print("INMP: samples=");
  Serial.print(n.samplesCount);
  Serial.print(" rms=");
  Serial.print(n.rms, 6);
  Serial.print(" dbfs=");
  if (isfinite(n.dbfs)) Serial.print(n.dbfs, 2);
  else Serial.print("-inf");
  Serial.print(" peak_dbfs=");
  if (isfinite(n.peak_dbfs)) Serial.print(n.peak_dbfs, 2);
  else Serial.print("-inf");
  Serial.print(" peak_sample=");
  Serial.print(n.peak_sample);
  Serial.print(" min=");
  Serial.print(n.min_sample);
  Serial.print(" max=");
  Serial.print(n.max_sample);
  Serial.print(" ts=");
  Serial.println(n.timestamp);
}
void scheduleInitialTimes() {
  unsigned long now = millis();
  // Force first immediate read on startup (set last to now - interval)
  lastDhtMillis = now - DHT_INTERVAL_MS;
  lastInmpMillis = now - INMP_INTERVAL_MS;
}

// New-style send callback (for esp_now_register_send_cb expecting wifi_tx_info_t)
// Simple new-style send callback that does NOT try to access MAC bytes
void on_send_cb(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // mark send complete
  last_send_status = status;
  send_in_progress = false;

  // Print send status (no MAC)
  Serial.print("on_send_cb -> ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void setup() {
  Serial.begin(115200);
  delay(100);
  while (!Serial && millis() < 5000) { delay(100); }  // wait a bit for Serial on some boards
  Serial.println("Starting sensors...");
  // Initialize sensors
  if (!dhtSensor.begin()) {
    Serial.println("DHTSensor.begin() failed (but continuing).");
  } else {
    Serial.println("DHTSensor initialized.");
  }

  if (!micSensor.begin()) {
    Serial.println("INMP441 begin failed.");
  } else {
    Serial.println("INMP441 initialized.");
  }
  // Initialize WIFI
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(1000);
  }
  esp_now_register_send_cb(on_send_cb);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);

  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(RECEIVER_MAC)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add peer");
    } else Serial.println("Peer added");
  } else Serial.println("Peer exists");

  Serial.print("Sender MAC: ");
  Serial.println(WiFi.macAddress());
  lastSendMs = millis() - 1000;
}

DHTResult r;
NoiseResult n;

void loop() {
  unsigned long now = millis();
  // DHT sampling
  if ((now - lastDhtMillis) >= DHT_INTERVAL_MS) {
    Serial.println("Starting DHT read...");
    r = dhtSensor.read();    // blocking but quick
    r.timestamp = millis();  // ensure timestamp updated after read
    Serial.println("DHT read complete:");
    printDHTResult(r);
    lastDhtMillis = now;
  }

  // INMP sampling
  if ((now - lastInmpMillis) >= INMP_INTERVAL_MS) {
    Serial.print("Starting INMP measurement for ");
    Serial.print(INMP_MEASURE_SECONDS);
    Serial.println(" seconds...");
    // measureNoise is blocking for the requested duration
    n = micSensor.measureNoise(INMP_MEASURE_SECONDS);
    n.timestamp = millis();
    Serial.println("INMP measurement complete:");
    printNoiseResult(n);
    lastInmpMillis = now;
  }

  // do other non-blocking tasks here
  if ((now - lastSendMs) >= SEND_INTERVAL_MS) {
    lastSendMs = now;

    if (send_in_progress) {
      Serial.println("Previous send still in progress; skipping this interval.");
      return;
    }

    // build logical payload
    WirePayload pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.version = 1;
    pkt.seq = ++send_seq;
    pkt.sensor_id = 4;  // set your node id

    // fill in sensor results
    pkt.dht = r;
    pkt.noise = n;

    // serialize into tx_buf and send only serialized bytes (including CRC)
    static uint8_t tx_buf[64];
    size_t len = serialize_WirePayload(pkt, tx_buf, sizeof(tx_buf));
    if (len == 0) {
      Serial.println("serialize failed: buffer too small");
      // decide how to handle this (skip send, increase buffer, etc.)
      return;
    }
    
    // print entire packet (payload+CRC)
    Serial.print("TX len=");
    Serial.println((unsigned)len);
    Serial.print("TX buf: ");
    print_hex_buf(tx_buf, len);

    // print CRC only (last 2 bytes are CRC LE)
    uint16_t tx_crc = (uint16_t)tx_buf[len - 2] | ((uint16_t)tx_buf[len - 1] << 8);
    Serial.print("TX CRC (raw LE)=0x");
    if (tx_crc < 0x1000) Serial.print('0');  // optional padding when using print HEX
    Serial.print(tx_crc, HEX);
    Serial.println();
    esp_err_t res = esp_now_send(RECEIVER_MAC, tx_buf, (int)len);
    if (res != ESP_OK) {
      Serial.print("esp_now_send error: ");
      Serial.println((int)res);
      send_in_progress = false;
    } else {
      Serial.print("Queued packet seq=");
      Serial.print(pkt.seq);
      Serial.print(" len=");
      Serial.println((unsigned)len);
      send_in_progress = true;  // cleared in send-complete callback
    }
  }
  delay(20);
}