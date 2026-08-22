#if 0
#include "esp_crc.h" // for esp_crc16_le on ESP32; optional fallback below
#include <string.h>
#include <stdint.h>
#include <stddef.h>

// --- low-level LE writers/readers ---
static void write_u8(uint8_t *buf, size_t &i, uint8_t v) { buf[i++] = v; }
static void write_i16_le(uint8_t *buf, size_t &i, int16_t v) {
buf[i++] = (uint8_t)(v & 0xFF); buf[i++] = (uint8_t)((v >> 8) & 0xFF);
}
static void write_u16_le(uint8_t *buf, size_t &i, uint16_t v) {
buf[i++] = (uint8_t)(v & 0xFF); buf[i++] = (uint8_t)((v >> 8) & 0xFF);
}
static void write_u32_le(uint8_t *buf, size_t &i, uint32_t v) {
buf[i++] = (uint8_t)(v & 0xFF); buf[i++] = (uint8_t)((v >> 8) & 0xFF);
buf[i++] = (uint8_t)((v >> 16) & 0xFF); buf[i++] = (uint8_t)((v >> 24) & 0xFF);
}
static void write_f32_le(uint8_t *buf, size_t &i, float v) {
uint32_t iv; memcpy(&iv, &v, 4);
write_u32_le(buf, i, iv);
}

static uint8_t read_u8(const uint8_t *buf, size_t &i) { return buf[i++]; }
static int16_t read_i16_le(const uint8_t *buf, size_t &i) {
int16_t v = (int16_t)(buf[i] | (buf[i+1] << 8)); i += 2; return v;
}
static uint16_t read_u16_le(const uint8_t *buf, size_t &i) {
uint16_t v = (uint16_t)(buf[i] | (buf[i+1] << 8)); i += 2; return v;
}
static uint32_t read_u32_le(const uint8_t *buf, size_t &i) {
uint32_t v = (uint32_t)buf[i] | ((uint32_t)buf[i+1] << 8) | ((uint32_t)buf[i+2] << 16) | ((uint32_t)buf[i+3] << 24);
i += 4; return v;
}
static float read_f32_le(const uint8_t *buf, size_t &i) {
uint32_t iv = read_u32_le(buf, i);
float f; memcpy(&f, &iv, 4); return f;
}

// --- CRC helper: prefer esp_crc16_le on ESP32, otherwise fallback ---
static uint16_t compute_crc16_le(const uint8_t *buf, size_t len) {
#ifdef ESP_PLATFORM
// esp_crc16_le(initial, data, len) expects initial CRC as first arg
return (uint16_t)esp_crc16_le(0xFFFF, buf, (uint32_t)len);
#else
// fallback: classic bitwise CRC16-CCITT with init 0xFFFF
uint16_t crc = 0xFFFF;
while (len--) {
crc ^= (uint16_t)(*buf++) << 8;
for (uint8_t j = 0; j < 8; ++j) {
if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
else crc <<= 1;
}
}
return crc;
#endif
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
if (buf_capacity < 16) return 0; // trivial guard
size_t idx = 0;
write_u8(buf, idx, pkt.version);
write_u16_le(buf, idx, pkt.seq);
write_i16_le(buf, idx, pkt.sensor_id);

idx = serialize_DHTResult(buf, idx, pkt.dht);
idx = serialize_NoiseResult(buf, idx, pkt.noise);

// compute CRC over bytes [0..idx-1]
uint16_t crc = compute_crc16_le(buf, idx);
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
uint16_t calc = compute_crc16_le(in_buf, payload_len);
if (calc != tx_crc) {
// CRC mismatch
return false;
}

size_t i = 0;
out_pkt.version = read_u8(in_buf, i);
out_pkt.seq = read_u16_le(in_buf, i);
out_pkt.sensor_id = read_i16_le(in_buf, i);
i = deserialize_DHTResult(in_buf, i, out_pkt.dht);
i = deserialize_NoiseResult(in_buf, i, out_pkt.noise);

// Optional sanity check: consumed payload bytes == payload_len
if (i != payload_len) {
// mismatch in expected sizes
return false;
}
// Set crc16 field if desired (extracted)
out_pkt.crc16 = tx_crc;
return true;
}

#endif