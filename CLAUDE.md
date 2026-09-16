# RTK Bridge — บันทึกส่งต่อให้ session ที่เครื่อง

โปรเจกต์นี้เขียนขึ้นใน Claude Code บนคลาวด์ ซึ่ง **คอมไพล์ไม่ได้** เพราะ network policy
บล็อกทั้ง PlatformIO registry และ `downloads.arduino.cc` (ทั้งคู่ตอบ 403)

**งานแรกของ session ที่เครื่อง: คอมไพล์ให้ผ่านก่อน** โค้ดยังไม่เคยผ่าน compiler เลยสักครั้ง
คาดว่าจะเจอ error ระดับชื่อ API ของ ESP-IDF ที่เพี้ยนไปตามเวอร์ชันของ core

## ฮาร์ดแวร์

- ESP32-WROOM-32 DevKit 38 pin, ชิป USB เป็น CP2102 (ไม่ใช่ WROVER — GPIO16/17 จึงว่าง)
- UM981 ต่อ UART2: GPIO16 = RX2, GPIO17 = TX2, 230400 baud, ต้องต่อ GND ร่วม
- ข้อมูลลง: NMEA GGA/RMC/VTG ที่ 10 Hz + GSV/GST/GSA ที่ 1 Hz ≈ 3-5 kB/s
- ข้อมูลขึ้น: RTCM จาก NTRIP บนมือถือ

## โครงสร้าง

`RTKBridge/` เป็น sketch folder ของ Arduino IDE (ชื่อโฟลเดอร์ต้องตรงกับ `.ino`)
และ `platformio.ini` ตั้ง `src_dir` ชี้มาที่เดียวกัน **มีซอร์สชุดเดียว ทั้งสอง toolchain ใช้ร่วมกัน**
อย่า copy ไฟล์ไปไว้อีกที่

## วิธี build / flash

```bash
tools/flash-and-watch.sh /dev/ttyUSB0 30     # arduino-cli
pio run -t upload && pio device monitor      # platformio
```

Arduino IDE: Board = ESP32 Dev Module, **Partition Scheme = Minimal SPIFFS (1.9MB APP)**
(BT Classic + WiFi ไม่พอดี partition default), Core Debug Level = **Info**

## จุดที่ต้องระวังตอนแก้

- **ห้ามใส่อะไรที่บล็อกลงใน `bridgeTask`** ทั้งเส้นทางข้อมูลออกแบบให้ไม่บล็อกโดยตั้งใจ
  เพราะการบล็อกคือต้นเหตุของบั๊กที่โปรเจกต์นี้แก้อยู่ ถ้าจะเขียนลง transport ให้เขียน
  เท่าที่มันรับได้ตอนนั้น แล้วทิ้งส่วนเกิน
- **อย่าย้อนกลับไปใช้ `BluetoothSerial.h`** มัน session เดียวแบบฮาร์ดโค้ด ซึ่งเป็นเหตุผล
  ที่ multi-device ทำไม่ได้ตั้งแต่แรก โค้ดนี้เรียก `esp_spp_api` ตรง
- `SppMulti::onSpp` รันใน **Bluedroid callback task** ห้ามบล็อก ห้ามเรียก Serial ยาวๆ
  งานจริงไปทำใน `poll()` ซึ่งอยู่ใน bridge task
- `setRxBufferSize()` / `setTxBufferSize()` ต้องเรียก **ก่อน** `begin()` ไม่งั้นเงียบหาย
- `ByteRing` มี unit test บน host แล้ว ถ้าแก้ตรรกะมันให้รันเทสต์ซ้ำ

## ทดสอบว่าแก้ปัญหาได้จริง

ต้องจำลองให้ตรงอาการ คือแอปตายแบบ **ไม่ปิด socket**:

```bash
adb shell am force-stop <package-name>
```

แล้วดู serial ต้องขึ้น `stalled ... - dropping it` ภายใน ~6 วินาที และต่อกลับได้ทันที
โดยไม่ต้องแตะบอร์ด (กดปิดแอปตามปกติ **ไม่นับ** เพราะ Android ปิด socket ให้เรียบร้อย)

## สถานะ

- ยังไม่เคยคอมไพล์
- `ByteRing` ผ่านเทสต์ host 6 เคสภายใต้ ASan/UBSan
- ยังไม่เคยแฟลชลงบอร์ดจริง
