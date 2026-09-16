#pragma once

#include <Arduino.h>
#include "config.h"

// Corrections flow the other way: phone (NTRIP) -> ESP32 -> UM981.
//
// With several clients attached only one may write, because two RTCM streams
// interleaved byte-wise would corrupt every frame. First to speak wins, and
// ownership lapses after UPLINK_OWNER_TIMEOUT_MS of silence so a client that
// disappears does not lock the receiver out forever.
//
// Claimed from both the Bluedroid callback task and the bridge task, hence the
// spinlock.
class UplinkArbiter {
 public:
  bool claim(uint32_t id, uint32_t nowMs) {
    bool granted;
    portENTER_CRITICAL(&_mux);
    if (_owner == 0 || _owner == id ||
        (uint32_t)(nowMs - _lastMs) > UPLINK_OWNER_TIMEOUT_MS) {
      _owner = id;
      _lastMs = nowMs;
      granted = true;
    } else {
      granted = false;
    }
    portEXIT_CRITICAL(&_mux);
    return granted;
  }

  void release(uint32_t id) {
    portENTER_CRITICAL(&_mux);
    if (_owner == id) _owner = 0;
    portEXIT_CRITICAL(&_mux);
  }

  uint32_t owner() const { return _owner; }

 private:
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  volatile uint32_t _owner = 0;
  volatile uint32_t _lastMs = 0;
};

// Client id space, so Bluetooth and TCP clients can share one arbiter.
// SPP ids are the stack's connection handle (never 0).
static inline uint32_t tcpClientId(uint8_t index) {
  return 0x80000000UL | index;
}
