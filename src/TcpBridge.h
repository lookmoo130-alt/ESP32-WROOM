#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "ByteRing.h"
#include "Uplink.h"
#include "config.h"

// TCP side of the bridge.
//
// Bluetooth Classic SPP caps out at whatever Bluedroid was compiled for
// (typically three sessions). If you actually need several displays on one
// receiver, this is the path that scales, and it gives us real dead-peer
// detection: TCP keepalive tears down a half-open socket in about ten seconds
// whether or not the phone ever closed it.
struct TcpSlot {
  WiFiClient client;
  ByteRing ring;
  uint32_t openedMs;
  uint32_t lastDrainMs;
  uint64_t bytesOut;
  uint64_t bytesIn;
  uint64_t bytesDropped;
  bool inUse;
};

class TcpBridge {
 public:
  bool begin(UplinkArbiter *arbiter, HardwareSerial *gnss);
  void poll(uint32_t nowMs);
  void broadcast(const uint8_t *data, size_t len);

  uint8_t activeCount() const;
  void printStatus(Stream &s) const;

 private:
  void accept(uint32_t nowMs);
  void adopt(TcpSlot &slot, WiFiClient &incoming, uint32_t nowMs);
  void drop(TcpSlot &slot, uint8_t index, const char *why);

  WiFiServer _server{TCP_PORT};
  TcpSlot _slots[TCP_MAX_CLIENTS];
  UplinkArbiter *_arbiter = nullptr;
  HardwareSerial *_gnss = nullptr;
  uint8_t _txBuf[TCP_TX_CHUNK];
};
