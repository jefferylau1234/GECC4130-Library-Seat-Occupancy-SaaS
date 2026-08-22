#pragma once
#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h> // for INT16_MAX in docs if needed

// Lightweight POD for DHT results
struct DHTResult {
  bool ok;             // true if read succeeded
  float temperature;   // degrees Celsius
  float humidity;      // percent RH
  uint32_t timestamp;  // millis() at read time (filled by implementation)
};

// Lightweight POD for microphone / noise results
struct NoiseResult {
  float rms;           // normalized RMS (0..1)
  float dbfs;          // dB relative to full-scale (dBFS)
  float peak_dbfs;     // peak in dBFS
  int16_t peak_sample; // sample value at peak
  int16_t min_sample;
  int16_t max_sample;
  uint32_t samplesCount;
  uint32_t timestamp;  // millis() when measurement finished
};

/*
  DHTSensor
  - Encapsulates a single DHT sensor (e.g. DHT11/DHT22).
  - Minimal public interface: construct, begin, read, end.
  - Implementation should live in DHTSensor.cpp (include Arduino.h and the DHT lib there).
*/
class DHTSensor {
public:
  // typeHint: pass sensor type constant (e.g. DHT11 or DHT22) from your DHT library.
  DHTSensor(int dataPin, int typeHint = 11); // default typeHint=11 (DHT11)
  ~DHTSensor();

  // Initialize hardware / library. Call from setup().
  // Returns true on success, false on failure.
  bool begin();

  // Stop / cleanup (optional).
  void end();

  // Read once (blocking). Returns DHTResult. If read failed, ok==false.
  DHTResult read();

  // Non-copyable
  DHTSensor(const DHTSensor&) = delete;
  DHTSensor& operator=(const DHTSensor&) = delete;

private:
  struct Impl;
  Impl* pimpl;
};

/*
  INMP441Sensor
  - Encapsulates an INMP441 I2S microphone on ESP32 (or other I2S-capable MCU).
  - Public API supports initialization, a blocking measureNoise() that records for a
    specified period and computes RMS / dBFS, and simple calibration storage.
  - Implementation should live in INMP441Sensor.cpp (include driver/i2s.h, Preferences.h, etc).
*/
class INMP441Sensor {
public:
  // Constructor: specify I2S pins and default sample rate (Hz).
  // i2s_bck: bit clock (BCK), i2s_ws: word select (LRCLK), i2s_sd: data in pin.
  INMP441Sensor(int i2s_bck = 26, int i2s_ws = 22, int i2s_sd = 21, uint32_t sampleRate = 16000);
  ~INMP441Sensor();

  // Initialize I2S driver and internal buffers. Call from setup().
  // Returns true on success.
  bool begin();

  // Deinitialize I2S driver and free resources.
  void end();

  // Blocking measurement: measure for samplePeriodSec seconds (default 5s).
  // Returns a NoiseResult. If no samples read, samplesCount==0 and dbfs may be -INFINITY or NAN.
  NoiseResult measureNoise(uint32_t samplePeriodSec = 5);

  // Calibration helpers: one-point calibration saves offset = referenceDb - measuredDbfs
  // Returns saved offset (dB) or NAN on failure.
  float calibrateOnePoint(float referenceDb);

  bool hasCalibration() const;
  float getCalibrationOffset() const;
  void clearCalibration();

  // Set/get sample rate. Changing sample rate typically requires reinitializing (end()/begin()).
  void setSampleRate(uint32_t sr);
  uint32_t getSampleRate() const;

  // Non-copyable
  INMP441Sensor(const INMP441Sensor&) = delete;
  INMP441Sensor& operator=(const INMP441Sensor&) = delete;

private:
  struct Impl;
  Impl* pimpl;
};

#endif // SENSORS_H