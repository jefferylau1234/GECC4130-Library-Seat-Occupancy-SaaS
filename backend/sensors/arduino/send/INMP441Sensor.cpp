#include "Sensors.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>

// Preferences keys
static const char* PREF_NS = "inmp441";
static const char* PREF_CAL_KEY = "cal_offset";
static const char* PREF_VALID_KEY = "cal_valid";

// Defaults (mirror header if you change header defaults)
static const uint32_t DEFAULT_SAMPLE_RATE = 16000;
static const int DEFAULT_I2S_BCK = 26;
static const int DEFAULT_I2S_WS  = 22;
static const int DEFAULT_I2S_SD  = 21;

// Helper: convert 32-bit left-justified sample to signed 16-bit PCM.
// Adjust if your microphone outputs a different packing.
static inline int16_t conv32to16(int32_t v32) {
  return (int16_t)(v32 >> 16);
}

// PIMPL for INMP441Sensor
struct INMP441Sensor::Impl {
  int i2s_bck;
  int i2s_ws;
  int i2s_sd;
  uint32_t sampleRate;
  i2s_port_t i2s_port;

  // buffer
  static constexpr size_t READ_WORDS = 1024; // number of 32-bit words per read
  int32_t* read_buf;

  // Preferences / calibration
  Preferences prefs;
  bool hasCal;
  float calOffsetDb;

  Impl(int bck, int ws, int sd, uint32_t sr)
    : i2s_bck(bck), i2s_ws(ws), i2s_sd(sd), sampleRate(sr), i2s_port(I2S_NUM_0),
      read_buf(nullptr), hasCal(false), calOffsetDb(0.0f) {}

  ~Impl() {
    if (read_buf) {
      free(read_buf);
      read_buf = nullptr;
    }
  }

  bool loadCalibration() {
    prefs.begin(PREF_NS, true);
    hasCal = prefs.getBool(PREF_VALID_KEY, false);
    if (hasCal) {
      calOffsetDb = prefs.getFloat(PREF_CAL_KEY, 0.0f);
    } else {
      calOffsetDb = 0.0f;
    }
    prefs.end();
    return true;
  }

  bool saveCalibration(float offsetDb) {
    prefs.begin(PREF_NS, false);
    prefs.putBool(PREF_VALID_KEY, true);
    prefs.putFloat(PREF_CAL_KEY, offsetDb);
    prefs.end();
    hasCal = true;
    calOffsetDb = offsetDb;
    return true;
  }

  bool clearCalibration() {
    prefs.begin(PREF_NS, false);
    prefs.putBool(PREF_VALID_KEY, false);
    prefs.putFloat(PREF_CAL_KEY, 0.0f);
    prefs.end();
    hasCal = false;
    calOffsetDb = 0.0f;
    return true;
  }

  bool i2sInit() {
    if (!read_buf) {
      read_buf = (int32_t*)malloc(READ_WORDS * sizeof(int32_t));
      if (!read_buf) return false;
    }

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = sampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_I2S_MSB;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 0;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = i2s_bck;
    pins.ws_io_num = i2s_ws;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = i2s_sd;

    esp_err_t err = i2s_driver_install(i2s_port, &cfg, 0, nullptr);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      return false;
    }
    err = i2s_set_pin(i2s_port, &pins);
    if (err != ESP_OK) return false;

    // ensure sample rate set
    i2s_set_sample_rates(i2s_port, sampleRate);
    return true;
  }

  void i2sDeinit() {
    i2s_driver_uninstall(i2s_port);
  }
};

INMP441Sensor::INMP441Sensor(int i2s_bck, int i2s_ws, int i2s_sd, uint32_t sampleRate)
  : pimpl(new Impl(i2s_bck, i2s_ws, i2s_sd, sampleRate))
{}

INMP441Sensor::~INMP441Sensor() {
  if (pimpl) {
    pimpl->i2sDeinit();
    delete pimpl;
    pimpl = nullptr;
  }
}

bool INMP441Sensor::begin() {
  if (!pimpl) return false;

  if (!pimpl->i2sInit()) {
    Serial.println("INMP441: i2s init failed");
    return false;
  }

  pimpl->loadCalibration();
  return true;
}

void INMP441Sensor::end() {
  if (!pimpl) return;
  pimpl->i2sDeinit();
}

NoiseResult INMP441Sensor::measureNoise(uint32_t samplePeriodSec) {
  NoiseResult res;
  res.rms = 0.0f;
  res.dbfs = -INFINITY;
  res.peak_dbfs = -INFINITY;
  res.peak_sample = 0;
  res.min_sample = 0;
  res.max_sample = 0;
  res.samplesCount = 0;
  res.timestamp = millis();

  if (!pimpl || !pimpl->read_buf) return res;

  const uint32_t totalMs = samplePeriodSec * 1000UL;
  const uint32_t startMs = millis();

  uint64_t sumSquares = 0ULL;
  uint32_t totalSamples = 0;
  int16_t minS = INT16_MAX;
  int16_t maxS = INT16_MIN;
  int16_t peakSample = 0;
  float peakDb = -INFINITY;

  size_t bytes_read = 0;
  const size_t word_bytes = sizeof(int32_t);

  while ((millis() - startMs) < totalMs) {
    esp_err_t err = i2s_read(pimpl->i2s_port, pimpl->read_buf, Impl::READ_WORDS * word_bytes, &bytes_read, pdMS_TO_TICKS(500));
    if (err != ESP_OK || bytes_read == 0) {
      delay(1);
      continue;
    }
    size_t samples_read = bytes_read / word_bytes;
    int32_t* src = pimpl->read_buf;
    for (size_t i = 0; i < samples_read; ++i) {
      int16_t s = conv32to16(src[i]);
      int32_t si = (int32_t)s;
      sumSquares += (uint64_t)(si * (int64_t)si);
      totalSamples++;

      if (s < minS) minS = s;
      if (s > maxS) maxS = s;

      int16_t absS = (s == INT16_MIN) ? INT16_MAX : (s < 0 ? -s : s);
      int16_t absPeak = (peakSample < 0) ? -peakSample : peakSample;
      if (absS > absPeak) {
        peakSample = s;
        float normalized = (float)absS / (float)INT16_MAX;
        float pd = (normalized > 0.0f) ? (20.0f * log10f(normalized)) : -INFINITY;
        if (pd > peakDb) peakDb = pd;
      }
    }
    yield();
  }

  if (totalSamples == 0) {
    res.samplesCount = 0;
    res.timestamp = millis();
    return res;
  }

  double meanSquare = (double)sumSquares / (double)totalSamples;
  double rms = sqrt(meanSquare);
  double normalizedRms = rms / (double)INT16_MAX;
  res.rms = (float)normalizedRms;
  if (normalizedRms > 0.0) {
    res.dbfs = 20.0f * log10f((float)normalizedRms);
  } else {
    res.dbfs = -INFINITY;
  }
  res.peak_dbfs = peakDb;
  res.peak_sample = peakSample;
  res.min_sample = minS;
  res.max_sample = maxS;
  res.samplesCount = totalSamples;
  res.timestamp = millis();

  // apply calibration offset if you want externally; we store it but do not auto-apply here.
  // Users can call getCalibrationOffset() and adjust dbfs if desired.

  return res;
}

float INMP441Sensor::calibrateOnePoint(float referenceDb) {
  if (!pimpl) return NAN;
  NoiseResult r = measureNoise(5);
  if (r.samplesCount == 0 || !isfinite(r.dbfs)) return NAN;
  float offset = referenceDb - r.dbfs;
  pimpl->saveCalibration(offset);
  return offset;
}

bool INMP441Sensor::hasCalibration() const {
  if (!pimpl) return false;
  return pimpl->hasCal;
}

float INMP441Sensor::getCalibrationOffset() const {
  if (!pimpl) return 0.0f;
  return pimpl->calOffsetDb;
}

void INMP441Sensor::clearCalibration() {
  if (!pimpl) return;
  pimpl->clearCalibration();
}

void INMP441Sensor::setSampleRate(uint32_t sr) {
  if (!pimpl) return;
  pimpl->sampleRate = sr;
  // Note: changing sample rate requires reinitialization (end()/begin()) to take effect.
}

uint32_t INMP441Sensor::getSampleRate() const {
  if (!pimpl) return DEFAULT_SAMPLE_RATE;
  return pimpl->sampleRate;
}