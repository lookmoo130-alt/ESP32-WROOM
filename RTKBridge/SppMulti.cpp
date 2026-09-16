#include "SppMulti.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"

static SppMulti *s_self = nullptr;

static void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (s_self) s_self->onSpp(event, param);
}

static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      log_i("BT auth %s", param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "ok" : "failed");
      break;
    default:
      break;
  }
}

static void setDeviceName(const char *name) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_bt_gap_set_device_name(name);
#else
  esp_bt_dev_set_device_name(name);
#endif
}

bool SppMulti::begin(UplinkArbiter *arbiter, StreamBufferHandle_t uplink) {
  s_self = this;
  _arbiter = arbiter;
  _uplink = uplink;
  _lastAnyDrainMs = millis();

  for (auto &slot : _slots) {
    // Field by field: ByteRing owns a heap pointer, so the struct is not POD.
    slot.handle = 0;
    slot.pendingOpen = false;
    slot.pendingClose = false;
    slot.congested = false;
    slot.writeBusy = false;
    slot.lastRxMs = 0;
    memset(slot.addr, 0, sizeof(slot.addr));
    slot.openedMs = slot.lastDrainMs = slot.kickedMs = 0;
    slot.kicked = false;
    slot.bytesOut = slot.bytesIn = slot.bytesDropped = 0;
    if (!slot.ring.begin(CLIENT_RING_SIZE)) {
      log_e("out of memory allocating SPP client queue");
      return false;
    }
  }

  // BLE is never used here; hand its controller memory back as free heap.
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

  esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  btCfg.mode = ESP_BT_MODE_CLASSIC_BT;
  if (esp_bt_controller_init(&btCfg) != ESP_OK) return false;
  if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) return false;
  if (esp_bluedroid_init() != ESP_OK) return false;
  if (esp_bluedroid_enable() != ESP_OK) return false;

  esp_bt_gap_register_callback(gapCallback);
  esp_spp_register_callback(sppCallback);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_spp_cfg_t sppCfg = {};
  sppCfg.mode = ESP_SPP_MODE_CB;
  sppCfg.enable_l2cap_ertm = false;
  sppCfg.tx_buffer_size = 0;
  if (esp_spp_enhanced_init(&sppCfg) != ESP_OK) return false;
#else
  if (esp_spp_init(ESP_SPP_MODE_CB) != ESP_OK) return false;
#endif

  // "Just works" pairing: no PIN prompt to get stuck behind on reconnect.
  esp_bt_io_cap_t ioCap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &ioCap, sizeof(uint8_t));

  setDeviceName(BT_DEVICE_NAME);
  return true;
}

// Re-assert discoverability. Some stacks quietly drop out of discoverable mode
// once a session is up, which on its own is enough to make a second device
// unable to find the board.
void SppMulti::advertise() {
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

SppSlot *SppMulti::slotByHandle(uint32_t handle) {
  if (handle == 0) return nullptr;
  for (auto &slot : _slots) {
    if (slot.handle == handle) return &slot;
  }
  return nullptr;
}

SppSlot *SppMulti::freeSlot() {
  for (auto &slot : _slots) {
    if (slot.handle == 0 && !slot.pendingOpen) return &slot;
  }
  return nullptr;
}

void SppMulti::releaseSlot(SppSlot &slot) {
  slot.handle = 0;
  slot.pendingOpen = false;
  slot.pendingClose = false;
  slot.congested = false;
  slot.writeBusy = false;
  slot.kicked = false;
  slot.ring.clear();
}

// Runs in the Bluedroid callback task: keep it short and never block.
void SppMulti::onSpp(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  switch (event) {
    case ESP_SPP_INIT_EVT:
      advertise();
      esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, SPP_SERVER_NAME);
      break;

    case ESP_SPP_START_EVT:
      advertise();
      break;

    case ESP_SPP_SRV_OPEN_EVT: {
      portENTER_CRITICAL(&_mux);
      SppSlot *slot = freeSlot();
      if (slot) {
        memcpy(slot->addr, param->srv_open.rem_bda, 6);
        slot->handle = param->srv_open.handle;
        slot->pendingOpen = true;
        slot->lastRxMs = millis();
      }
      portEXIT_CRITICAL(&_mux);

      if (!slot) {
        _rejected++;
        esp_spp_disconnect(param->srv_open.handle);
        break;
      }
      // Stay connectable so the next device is not locked out by this one.
      advertise();
      break;
    }

    case ESP_SPP_CLOSE_EVT: {
      SppSlot *slot = slotByHandle(param->close.handle);
      if (slot) slot->pendingClose = true;
      advertise();
      break;
    }

    case ESP_SPP_DATA_IND_EVT: {
      SppSlot *slot = slotByHandle(param->data_ind.handle);
      if (!slot) break;
      slot->lastRxMs = millis();
      slot->bytesIn += param->data_ind.len;
      if (_arbiter && _arbiter->claim(slot->handle, millis())) {
        // Drop rather than block: a full uplink buffer means the receiver is
        // not keeping up, and stalling this task would stall all of Bluetooth.
        xStreamBufferSend(_uplink, param->data_ind.data, param->data_ind.len, 0);
      }
      break;
    }

    case ESP_SPP_CONG_EVT: {
      SppSlot *slot = slotByHandle(param->cong.handle);
      if (slot) slot->congested = param->cong.cong;
      break;
    }

    case ESP_SPP_WRITE_EVT: {
      SppSlot *slot = slotByHandle(param->write.handle);
      if (!slot) break;
      slot->writeBusy = false;
      slot->congested = param->write.cong;
      if (param->write.status == ESP_SPP_SUCCESS) {
        slot->bytesOut += param->write.len;
      }
      break;
    }

    default:
      break;
  }
}

void SppMulti::broadcast(const uint8_t *data, size_t len) {
  for (auto &slot : _slots) {
    if (slot.handle == 0 || slot.pendingClose) continue;
    slot.bytesDropped += slot.ring.pushLine(data, len);
  }
}

void SppMulti::poll(uint32_t nowMs) {
  bool anyActive = false;

  for (auto &slot : _slots) {
    if (slot.pendingOpen) {
      slot.pendingOpen = false;
      slot.openedMs = nowMs;
      slot.lastDrainMs = nowMs;
      slot.kicked = false;
      slot.congested = false;
      slot.writeBusy = false;
      slot.ring.clear();  // a recycled slot must not leak the old peer's data
      log_i("SPP client %02x:%02x:%02x:%02x:%02x:%02x connected (handle %u)",
            slot.addr[0], slot.addr[1], slot.addr[2], slot.addr[3], slot.addr[4],
            slot.addr[5], (unsigned)slot.handle);
    }

    if (slot.pendingClose) {
      uint32_t handle = slot.handle;
      bool released = false;
      portENTER_CRITICAL(&_mux);
      // pendingOpen means the callback has already handed this slot to a new
      // peer; tearing it down now would drop that fresh session on the floor.
      if (slot.pendingClose && !slot.pendingOpen) {
        releaseSlot(slot);
        released = true;
      }
      portEXIT_CRITICAL(&_mux);
      if (released) {
        log_i("SPP client handle %u disconnected", (unsigned)handle);
        if (_arbiter) _arbiter->release(handle);
        continue;
      }
    }

    if (slot.handle == 0) continue;
    anyActive = true;

    // Hand the stack at most one chunk at a time and only when it says it has
    // room. Nothing here can block, which is the whole point.
    if (!slot.writeBusy && !slot.congested && !slot.ring.empty()) {
      size_t n = slot.ring.peek(slot.inflight, SPP_TX_CHUNK, LINE_FRAMED_DOWNLINK);
      if (n > 0) {
        uint32_t handle = slot.handle;
        slot.writeBusy = true;
        if (esp_spp_write(handle, n, slot.inflight) == ESP_OK) {
          slot.ring.consume(n);
          slot.lastDrainMs = nowMs;
          _lastAnyDrainMs = nowMs;
        } else {
          slot.writeBusy = false;
        }
      }
    }

    if (slot.ring.empty() && !slot.writeBusy) slot.lastDrainMs = nowMs;

    // A live peer always drains. One that has accepted nothing for
    // STALL_TIMEOUT_MS is a crashed app or a sleeping phone, and its slot is
    // worth more to the next device than to it.
    bool stalled = (uint32_t)(nowMs - slot.lastDrainMs) > STALL_TIMEOUT_MS;
    if (stalled && !slot.kicked) {
      log_w("SPP client handle %u stalled for %u ms - dropping it",
            (unsigned)slot.handle, (unsigned)(nowMs - slot.lastDrainMs));
      slot.kicked = true;
      slot.kickedMs = nowMs;
      esp_spp_disconnect(slot.handle);
      advertise();
    }

    // If the close event never arrives, reclaim the slot anyway rather than
    // leak it - a leaked slot is exactly the lockout we are fixing.
    if (slot.kicked && (uint32_t)(nowMs - slot.kickedMs) > KICK_GRACE_MS) {
      uint32_t handle = slot.handle;
      bool released = false;
      portENTER_CRITICAL(&_mux);
      if (slot.kicked && !slot.pendingOpen) {
        releaseSlot(slot);
        released = true;
      }
      portEXIT_CRITICAL(&_mux);
      if (released) {
        log_w("SPP handle %u never closed - reclaiming slot", (unsigned)handle);
        if (_arbiter) _arbiter->release(handle);
        advertise();
      }
    }
  }

  // "Wedged" means there is data waiting and none of it is moving. A receiver
  // that has simply gone quiet leaves empty queues, and must not trigger the
  // reboot.
  bool backlog = false;
  for (const auto &slot : _slots) {
    if (slot.handle != 0 && (!slot.ring.empty() || slot.writeBusy)) backlog = true;
  }
  if (!anyActive || !backlog) _lastAnyDrainMs = nowMs;
}

uint8_t SppMulti::activeCount() const {
  uint8_t n = 0;
  for (const auto &slot : _slots) {
    if (slot.handle != 0) n++;
  }
  return n;
}

bool SppMulti::wedged(uint32_t nowMs) const {
  if (BT_WEDGE_REBOOT_MS == 0) return false;
  return (uint32_t)(nowMs - _lastAnyDrainMs) > BT_WEDGE_REBOOT_MS;
}

void SppMulti::printStatus(Stream &s) const {
  for (uint8_t i = 0; i < SPP_MAX_CLIENTS; i++) {
    const SppSlot &slot = _slots[i];
    if (slot.handle == 0) continue;
    s.printf("  SPP[%u] %02x:%02x:%02x:%02x:%02x:%02x out=%llu in=%llu dropped=%llu q=%u%s\n",
             i, slot.addr[0], slot.addr[1], slot.addr[2], slot.addr[3], slot.addr[4],
             slot.addr[5], (unsigned long long)slot.bytesOut,
             (unsigned long long)slot.bytesIn, (unsigned long long)slot.bytesDropped,
             (unsigned)slot.ring.used(), slot.congested ? " CONGESTED" : "");
  }
  if (_rejected) s.printf("  SPP rejected (table full): %u\n", (unsigned)_rejected);
}
