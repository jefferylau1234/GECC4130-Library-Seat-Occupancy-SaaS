#include "Sensors.h"
#include <Arduino.h>
#include <DHT.h>

// PIMPL for DHTSensor
struct DHTSensor::Impl {
  int dataPin;
  int typeHint; // e.g. DHT11 = 11, DHT22 = 22
  DHT* dht;

  Impl(int pin, int type)
    : dataPin(pin), typeHint(type), dht(nullptr) {}

  ~Impl() {
    if (dht) {
      delete dht;
      dht = nullptr;
    }
  }
};

DHTSensor::DHTSensor(int dataPin, int typeHint)
  : pimpl(new Impl(dataPin, typeHint))
{}

DHTSensor::~DHTSensor() {
  delete pimpl;
  pimpl = nullptr;
}

bool DHTSensor::begin() {
  if (!pimpl) return false;
  // Create DHT instance and begin. The DHT class constructor accepts (pin, type).
  pimpl->dht = new DHT(pimpl->dataPin, (pimpl->typeHint == 22) ? DHT22 : DHT11);
  pimpl->dht->begin();
  // We have no clear "init failure" indicator from Adafruit DHT library, so assume success.
  return true;
}

void DHTSensor::end() {
  if (!pimpl) return;
  if (pimpl->dht) {
    delete pimpl->dht;
    pimpl->dht = nullptr;
  }
}

DHTResult DHTSensor::read() {
  DHTResult res;
  res.ok = false;
  res.temperature = NAN;
  res.humidity = NAN;
  res.timestamp = millis();

  if (!pimpl || !pimpl->dht) {
    return res;
  }

  // Adafruit DHT returns NAN on failure
  float t = pimpl->dht->readTemperature();
  float h = pimpl->dht->readHumidity();

  if (!isnan(t) && !isnan(h)) {
    res.ok = true;
    res.temperature = t;
    res.humidity = h;
  } else {
    res.ok = false;
    res.temperature = NAN;
    res.humidity = NAN;
  }
  res.timestamp = millis();
  return res;
}