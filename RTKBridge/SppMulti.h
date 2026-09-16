#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include "ByteRing.h"
#include "Uplink.h"
#include "config.h"
#include "esp_idf_version.h"
#include "esp_spp_api.h"

// Multi-session Bluetooth Classic SPP server.
//
// Arduino's BluetoothSerial wraps a single hardcoded session, which is exactly
// why a crashed app locks the board out: the one slot stays occupied and no
// second device can take its place. This talks to esp_spp_api directly so the
// stack may hand us several concurrent sessions, and it reclaims slots from
// dead peers instead of waiting on a link supervision timeout that may never
// fire.
struct SppSlot {
  volatile uint32_t handle;      // 0 = free
  volatile bool pendingOpen;     // set by the callback, finalised by poll()
  volatile bool pendingClose;
  volatile bool congested;
  volatile bool writeBusy;       // one outstanding esp_spp_write per handle
  volatile uint32_t lastRxMs;

  uint8_t addr[6];
  ByteRing ring;
  uint8_t inflight[SPP_TX_CHUNK];  // must stay valid until ESP_SPP_WRITE_EVT

  uint32_t openedMs;
  uint32_t lastDrainMs;
  uint32_t kickedMs;
  bool kicked;

  uint64_t bytesOut;
  uint64_t bytesIn;
  uint64_t bytesDropped;
};

class SppMulti {
 public:
  bool begin(UplinkArbiter *arbiter, StreamBufferHandle_t uplink);
  void poll(uint32_t nowMs);
  void broadcast(const uint8_t *data, size_t len);

  uint8_t activeCount() const;
  bool wedged(uint32_t nowMs) const;
  void printStatus(Stream &s) const;

  // Public only because the ESP-IDF callback is a bare C function pointer.
  void onSpp(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

 private:
  SppSlot *slotByHandle(uint32_t handle);
  SppSlot *freeSlot();
  SppSlot *stalestSlot(uint32_t nowMs);
  void releaseSlot(SppSlot &slot);
  void advertise();

  // Slots are allocated by the Bluedroid callback task and released by the
  // bridge task, so the handful of stores that move a slot between those two
  // states have to be atomic with respect to each other.
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  SppSlot _slots[SPP_MAX_CLIENTS];
  UplinkArbiter *_arbiter = nullptr;
  StreamBufferHandle_t _uplink = nullptr;
  uint32_t _lastAnyDrainMs = 0;
  uint32_t _rejected = 0;
  uint32_t _evicted = 0;
};
