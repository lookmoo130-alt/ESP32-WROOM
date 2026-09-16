// RTK Bridge - UM981 <-> Android display, on an ESP32-WROOM-32.
//
// The problem this firmware exists to solve: when the Android app crashes or is
// killed, it does not close its socket cleanly. The board never learns the peer
// is gone, keeps pushing 10 Hz NMEA into a queue nobody drains, blocks inside
// the transport write, stops servicing the UART, and ends up wedged with its
// one connection slot held by a corpse. No amount of retry logic on the phone
// can fix that, because the board is already dead.
//
// Three rules keep it alive:
//   1. Nothing on the data path may block. A slow peer loses old sentences.
//   2. A peer that stops draining is dropped, we do not wait for the link
//      supervision timeout to notice.
//   3. A new device is always allowed in, evicting the stalest session if it
//      has to. Newest wins.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include "ByteRing.h"
#include "SppMulti.h"
#include "Uplink.h"
#include "config.h"
#include "esp_task_wdt.h"

#if ENABLE_WIFI_BRIDGE
#include "TcpBridge.h"
#endif

static HardwareSerial GNSS(GNSS_UART_NUM);
static UplinkArbiter g_arbiter;
static StreamBufferHandle_t g_sppUplink = nullptr;
static SppMulti g_spp;
#if ENABLE_WIFI_BRIDGE
static TcpBridge g_tcp;
#endif

static uint64_t g_uartBytes = 0;
static uint64_t g_uplinkBytes = 0;
static uint32_t g_overlongLines = 0;

static void startWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

static void fanOut(const uint8_t *data, size_t len) {
  g_spp.broadcast(data, len);
#if ENABLE_WIFI_BRIDGE
  g_tcp.broadcast(data, len);
#endif
}

// Drain whatever the Bluedroid callback task queued for the receiver. Bounded
// by the UART's own free space so this can never block either.
static void drainSppUplink() {
  int room = GNSS.availableForWrite();
  while (room > 0) {
    uint8_t buf[256];
    size_t want = room < (int)sizeof(buf) ? (size_t)room : sizeof(buf);
    size_t n = xStreamBufferReceive(g_sppUplink, buf, want, 0);
    if (n == 0) break;
    GNSS.write(buf, n);
    g_uplinkBytes += n;
    room -= (int)n;
  }
}

static void printStatus(uint32_t nowMs) {
  uint8_t spp = g_spp.activeCount();
#if ENABLE_WIFI_BRIDGE
  uint8_t tcp = g_tcp.activeCount();
#else
  uint8_t tcp = 0;
#endif
  Serial.printf("[%lus] clients spp=%u tcp=%u | uart_in=%llu uplink=%llu overlong=%u | heap=%u\n",
                (unsigned long)(nowMs / 1000), spp, tcp,
                (unsigned long long)g_uartBytes, (unsigned long long)g_uplinkBytes,
                (unsigned)g_overlongLines, (unsigned)ESP.getFreeHeap());
  g_spp.printStatus(Serial);
#if ENABLE_WIFI_BRIDGE
  g_tcp.printStatus(Serial);
#endif
}

static void updateLed(uint32_t nowMs, uint8_t clients) {
  // dark: nobody attached. slow blink: one or more clients. fast: stalling.
  uint32_t period = clients ? 1000 : 0;
  if (!period) {
    digitalWrite(STATUS_LED_PIN, LOW);
    return;
  }
  digitalWrite(STATUS_LED_PIN, (nowMs % period) < (period / 2) ? HIGH : LOW);
}

static void bridgeTask(void *) {
  startWatchdog();

  static uint8_t rx[1024];
  static uint8_t line[MAX_LINE_LEN];
  size_t lineLen = 0;
  uint32_t lastStatus = 0;

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();

    // 1. Drain the UART first and unconditionally. Everything downstream is
    //    allowed to drop data; this is the one step that never may.
    int avail = GNSS.available();
    while (avail > 0) {
      size_t want = avail < (int)sizeof(rx) ? (size_t)avail : sizeof(rx);
      size_t n = GNSS.readBytes(rx, want);
      if (n == 0) break;
      avail -= n;
      g_uartBytes += n;

#if LINE_FRAMED_DOWNLINK
      for (size_t i = 0; i < n; i++) {
        line[lineLen++] = rx[i];
        if (rx[i] == '\n') {
          fanOut(line, lineLen);
          lineLen = 0;
        } else if (lineLen == sizeof(line)) {
          // Longer than any NMEA sentence: pass it through rather than lose it,
          // and count it so a misconfigured receiver is visible in the log.
          g_overlongLines++;
          fanOut(line, lineLen);
          lineLen = 0;
        }
      }
#else
      fanOut(rx, n);
#endif
    }

    // 2. Service the transports. Neither call blocks.
    g_spp.poll(now);
#if ENABLE_WIFI_BRIDGE
    g_tcp.poll(now);
#endif

    // 3. Corrections back to the receiver.
    drainSppUplink();

    // 4. Health.
    if (g_spp.wedged(now)) {
      Serial.println("Bluetooth stack has not drained for too long - rebooting");
      Serial.flush();
      ESP.restart();
    }

    uint8_t clients = g_spp.activeCount();
#if ENABLE_WIFI_BRIDGE
    clients += g_tcp.activeCount();
#endif
    updateLed(now, clients);

    if (now - lastStatus >= STATUS_PERIOD_MS) {
      lastStatus = now;
      printStatus(now);
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nRTK Bridge starting");

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Both must be set before begin() or they are ignored.
  GNSS.setRxBufferSize(GNSS_RX_BUFFER);
  GNSS.setTxBufferSize(GNSS_TX_BUFFER);
  GNSS.begin(GNSS_BAUD, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);
  GNSS.setTimeout(5);  // readBytes must never sit on the default 1 s timeout
  Serial.printf("UM981 UART%d @ %d (rx=%d tx=%d)\n", GNSS_UART_NUM, GNSS_BAUD, GNSS_RX_PIN,
                GNSS_TX_PIN);

  g_sppUplink = xStreamBufferCreate(UPLINK_BUFFER_SIZE, 1);
  if (!g_sppUplink) {
    Serial.println("failed to allocate uplink buffer");
    ESP.restart();
  }

  if (!g_spp.begin(&g_arbiter, g_sppUplink)) {
    Serial.println("Bluetooth SPP failed to start");
    ESP.restart();
  }
  Serial.printf("Bluetooth SPP up as \"%s\" (up to %d sessions)\n", BT_DEVICE_NAME,
                SPP_MAX_CLIENTS);

#if ENABLE_WIFI_BRIDGE
  if (!g_tcp.begin(&g_arbiter, &GNSS)) {
    Serial.println("TCP bridge failed to start");
  } else {
    Serial.printf("WiFi AP \"%s\" -> %s:%d\n", WIFI_AP_SSID,
                  WiFi.softAPIP().toString().c_str(), TCP_PORT);
  }
#endif

  // Pinned to core 1, above the Arduino loop task, so the radio stacks on core
  // 0 cannot starve the UART reader.
  xTaskCreatePinnedToCore(bridgeTask, "bridge", 6144, nullptr, 5, nullptr, 1);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
