#include "TcpBridge.h"

#include <errno.h>
#include <lwip/sockets.h>

// Ask lwIP to probe an idle peer and give up quickly. Without this a phone that
// crashed or walked out of range holds its socket open for hours, which is the
// TCP flavour of the same lockout Bluetooth has.
static void enableKeepalive(WiFiClient &client) {
  int fd = client.fd();
  if (fd < 0) return;
  int on = 1, idle = 5, interval = 2, count = 3;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

bool TcpBridge::begin(UplinkArbiter *arbiter, HardwareSerial *gnss) {
  _arbiter = arbiter;
  _gnss = gnss;

  for (auto &slot : _slots) {
    slot.inUse = false;
    slot.openedMs = slot.lastDrainMs = 0;
    slot.bytesOut = slot.bytesIn = slot.bytesDropped = 0;
    if (!slot.ring.begin(CLIENT_RING_SIZE)) {
      log_e("out of memory allocating TCP client queue");
      return false;
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, TCP_MAX_CLIENTS);
  WiFi.setSleep(false);  // sleep adds latency this stream cannot absorb

  _server.begin();
  _server.setNoDelay(true);

  log_i("TCP bridge on %s:%d (SSID %s)", WiFi.softAPIP().toString().c_str(), TCP_PORT,
        WIFI_AP_SSID);
  return true;
}

void TcpBridge::adopt(TcpSlot &slot, WiFiClient &incoming, uint32_t nowMs) {
  slot.client = incoming;
  slot.client.setNoDelay(true);
  enableKeepalive(slot.client);
  slot.ring.clear();
  slot.inUse = true;
  slot.openedMs = nowMs;
  slot.lastDrainMs = nowMs;
}

void TcpBridge::drop(TcpSlot &slot, uint8_t index, const char *why) {
  log_i("TCP client %u dropped (%s)", index, why);
  slot.client.stop();
  slot.inUse = false;
  slot.ring.clear();
  if (_arbiter) _arbiter->release(tcpClientId(index));
}

void TcpBridge::accept(uint32_t nowMs) {
  while (_server.hasClient()) {
    WiFiClient incoming = _server.accept();
    if (!incoming) continue;

    TcpSlot *target = nullptr;
    uint8_t targetIndex = 0;
    for (uint8_t i = 0; i < TCP_MAX_CLIENTS; i++) {
      if (!_slots[i].inUse) {
        target = &_slots[i];
        targetIndex = i;
        break;
      }
    }

    // Newest wins. A device knocking on the door is proof it wants a session
    // now; the stalest existing client is the better thing to give up.
    if (!target) {
      uint32_t stalest = 0;
      for (uint8_t i = 0; i < TCP_MAX_CLIENTS; i++) {
        uint32_t idleFor = nowMs - _slots[i].lastDrainMs;
        if (!target || idleFor > stalest) {
          stalest = idleFor;
          target = &_slots[i];
          targetIndex = i;
        }
      }
      drop(*target, targetIndex, "evicted for a new client");
    }

    adopt(*target, incoming, nowMs);
    log_i("TCP client %u connected from %s", targetIndex,
          target->client.remoteIP().toString().c_str());
  }
}

void TcpBridge::broadcast(const uint8_t *data, size_t len) {
  for (auto &slot : _slots) {
    if (!slot.inUse) continue;
    slot.bytesDropped += slot.ring.pushLine(data, len);
  }
}

void TcpBridge::poll(uint32_t nowMs) {
  accept(nowMs);

  for (uint8_t i = 0; i < TCP_MAX_CLIENTS; i++) {
    TcpSlot &slot = _slots[i];
    if (!slot.inUse) continue;

    if (!slot.client.connected()) {
      drop(slot, i, "peer closed");
      continue;
    }

    // Uplink: RTCM corrections from the phone straight to the receiver.
    int pending = slot.client.available();
    while (pending > 0) {
      uint8_t buf[256];
      int want = pending < (int)sizeof(buf) ? pending : (int)sizeof(buf);
      int n = slot.client.read(buf, want);
      if (n <= 0) break;
      pending -= n;
      slot.bytesIn += n;
      if (_arbiter && _arbiter->claim(tcpClientId(i), nowMs) && _gnss) {
        int room = _gnss->availableForWrite();
        if (room > 0) _gnss->write(buf, n < room ? n : room);
      }
    }

    // Downlink. WiFiClient::write() can block until the socket drains, which
    // is precisely the failure we are engineering out, so go to the socket
    // directly and take only what it will accept without waiting.
    if (!slot.ring.empty()) {
      size_t n = slot.ring.peek(_txBuf, sizeof(_txBuf), LINE_FRAMED_DOWNLINK);
      int fd = slot.client.fd();
      if (n > 0 && fd >= 0) {
        int written = ::send(fd, _txBuf, n, MSG_DONTWAIT);
        if (written > 0) {
          slot.ring.consume(written);
          slot.bytesOut += written;
          slot.lastDrainMs = nowMs;
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          drop(slot, i, "send failed");
          continue;
        }
      }
    }

    if (slot.ring.empty()) slot.lastDrainMs = nowMs;

    if ((uint32_t)(nowMs - slot.lastDrainMs) > STALL_TIMEOUT_MS) {
      drop(slot, i, "stalled - not draining");
    }
  }
}

uint8_t TcpBridge::activeCount() const {
  uint8_t n = 0;
  for (const auto &slot : _slots) {
    if (slot.inUse) n++;
  }
  return n;
}

void TcpBridge::printStatus(Stream &s) const {
  for (uint8_t i = 0; i < TCP_MAX_CLIENTS; i++) {
    const TcpSlot &slot = _slots[i];
    if (!slot.inUse) continue;
    s.printf("  TCP[%u] out=%llu in=%llu dropped=%llu q=%u\n", i,
             (unsigned long long)slot.bytesOut, (unsigned long long)slot.bytesIn,
             (unsigned long long)slot.bytesDropped, (unsigned)slot.ring.used());
  }
}
