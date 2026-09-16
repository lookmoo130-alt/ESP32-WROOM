#pragma once

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

// Per-client transmit queue.
//
// The bridge must never block on a slow or dead peer, so when a queue fills up
// the OLDEST data is evicted instead of stalling the writer. For NMEA that is
// the correct trade-off: a two second old fix is worthless, and keeping the
// newest sentences beats backing pressure up into the UART until its FIFO
// overflows.
//
// Data goes in one whole line at a time and comes back out on line boundaries,
// so a client never receives a torn sentence.
//
// Owned by a single task; no internal locking.
class ByteRing {
 public:
  bool begin(size_t capacity) {
    _buf = (uint8_t *)malloc(capacity);
    if (!_buf) return false;
    _cap = capacity;
    _head = _tail = 0;
    return true;
  }

  void clear() { _head = _tail = 0; }
  size_t capacity() const { return _cap; }
  bool empty() const { return _head == _tail; }

  size_t used() const {
    return (_head >= _tail) ? (_head - _tail) : (_cap - _tail + _head);
  }
  size_t room() const { return _cap ? _cap - used() - 1 : 0; }

  // Append one complete line, evicting whole lines from the front until it
  // fits. Returns how many bytes were dropped to make room.
  size_t pushLine(const uint8_t *data, size_t len) {
    if (!_buf || len == 0) return 0;
    if (len > _cap - 1) return len;  // single line larger than the queue
    size_t dropped = 0;
    while (room() < len) dropped += dropOldestLine();
    writeRaw(data, len);
    return dropped;
  }

  // Copy out up to maxLen bytes WITHOUT consuming them, so the caller can hand
  // the buffer to a transport and only consume once it was accepted. With
  // lineAligned the window is trimmed back to the last '\n'.
  size_t peek(uint8_t *out, size_t maxLen, bool lineAligned) const {
    size_t avail = used();
    size_t n = avail < maxLen ? avail : maxLen;
    if (n == 0) return 0;

    size_t first = _cap - _tail;
    if (first > n) first = n;
    memcpy(out, _buf + _tail, first);
    if (n > first) memcpy(out + first, _buf, n - first);

    // Only the tail of the window can split a line: everything in the ring was
    // pushed as whole lines, so a full drain always ends on '\n'.
    if (lineAligned && n < avail) {
      size_t cut = 0;
      for (size_t i = n; i > 0; --i) {
        if (out[i - 1] == '\n') {
          cut = i;
          break;
        }
      }
      if (cut) n = cut;  // no newline in the window: pass the raw chunk through
    }
    return n;
  }

  void consume(size_t n) {
    size_t avail = used();
    if (n > avail) n = avail;
    _tail = (_tail + n) % _cap;
  }

 private:
  void writeRaw(const uint8_t *data, size_t len) {
    size_t first = _cap - _head;
    if (first > len) first = len;
    memcpy(_buf + _head, data, first);
    if (len > first) memcpy(_buf, data + first, len - first);
    _head = (_head + len) % _cap;
  }

  size_t dropOldestLine() {
    size_t avail = used();
    if (avail == 0) return 0;
    for (size_t i = 0; i < avail; ++i) {
      if (_buf[(_tail + i) % _cap] == '\n') {
        consume(i + 1);
        return i + 1;
      }
    }
    consume(avail);  // no newline present at all
    return avail;
  }

  uint8_t *_buf = nullptr;
  size_t _cap = 0;
  size_t _head = 0;
  size_t _tail = 0;
};
