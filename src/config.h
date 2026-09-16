#pragma once

// ---------------------------------------------------------------------------
// UART to the UM981 receiver
// ---------------------------------------------------------------------------
#define GNSS_UART_NUM     2
#define GNSS_RX_PIN       16      // ESP32 RX2 <- UM981 TX
#define GNSS_TX_PIN       17      // ESP32 TX2 -> UM981 RX
#define GNSS_BAUD         230400

// The stock HardwareSerial RX buffer is 256 bytes. At 230400 baud that is
// filled in ~11 ms, so any hiccup in the bridge loop corrupts sentences.
// Must be set before begin().
#define GNSS_RX_BUFFER    8192
#define GNSS_TX_BUFFER    2048

// ---------------------------------------------------------------------------
// Bluetooth Classic (SPP)
// ---------------------------------------------------------------------------
#define BT_DEVICE_NAME    "RTK-Bridge"
#define SPP_SERVER_NAME   "RTK"

// Table size. The real ceiling is BTA_JV_MAX_RFC_SR_SESSION in the Bluedroid
// build shipped with the core (usually 3); extra slots simply stay empty.
#define SPP_MAX_CLIENTS   4
#define SPP_TX_CHUNK      512     // well under the ~990 byte SPP MTU

// ---------------------------------------------------------------------------
// WiFi bridge - the dependable path to many simultaneous devices.
// Set to 0 to build a Bluetooth-only firmware.
// ---------------------------------------------------------------------------
#define ENABLE_WIFI_BRIDGE 1
#define WIFI_AP_SSID       "RTK-Bridge"
#define WIFI_AP_PASSWORD   "rtk123456"   // must be >= 8 characters
#define WIFI_AP_CHANNEL    6
#define TCP_PORT           2947
#define TCP_MAX_CLIENTS    4
#define TCP_TX_CHUNK       1024

// ---------------------------------------------------------------------------
// Flow control and health
// ---------------------------------------------------------------------------

// Per-client transmit queue. ~4 KB buys roughly a second of slack at the
// 3-5 kB/s this stream runs at, which is all a real-time fix is worth.
#define CLIENT_RING_SIZE   4096

// A live peer always drains. If a client accepts nothing for this long it is
// dead (crashed app, phone asleep) and gets disconnected so its slot frees up.
#define STALL_TIMEOUT_MS   6000

// After we ask the stack to drop a client, give it this long before we reclaim
// the slot ourselves - a close event that never arrives must not leak a slot.
#define KICK_GRACE_MS      3000

// Last resort: clients are attached but nothing has drained for this long, so
// the Bluetooth stack itself is wedged. Reboot rather than stay dead in the
// field. Set to 0 to disable.
#define BT_WEDGE_REBOOT_MS 45000

// Only one client may feed RTCM upstream at a time; ownership lapses after
// this much silence.
#define UPLINK_OWNER_TIMEOUT_MS 10000
#define UPLINK_BUFFER_SIZE 4096

// The downlink is NMEA text, so frame it on newlines and never hand a client
// half a sentence. Set to 0 if you configure the UM981 for binary output.
#define LINE_FRAMED_DOWNLINK 1
#define MAX_LINE_LEN       512

#define WDT_TIMEOUT_S      10
#define STATUS_PERIOD_MS   5000
#define STATUS_LED_PIN     2
