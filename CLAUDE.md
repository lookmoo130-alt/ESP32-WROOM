# RTK Bridge — บันทึกส่งต่อให้ session ที่เครื่อง

โปรเจกต์นี้เขียนขึ้นใน Claude Code บนคลาวด์ ซึ่ง **คอมไพล์ไม่ได้** เพราะ network policy
บล็อกทั้ง PlatformIO registry และ `downloads.arduino.cc` (ทั้งคู่ตอบ 403)

**งานแรกของ session ที่เครื่อง: คอมไพล์ให้ผ่านก่อน** โค้ดยังไม่เคยผ่าน compiler เลยสักครั้ง
คาดว่าจะเจอ error ระดับชื่อ API ของ ESP-IDF ที่เพี้ยนไปตามเวอร์ชันของ core

## ฮาร์ดแวร์

- ESP32-WROOM-32 DevKit 38 pin, ชิป USB เป็น CP2102 (ไม่ใช่ WROVER — GPIO16/17 จึงว่าง)
- UM981 ต่อ UART2 แบบไขว้: UM981 TX → GPIO16 (RX2), UM981 RX → GPIO17 (TX2),
  230400 baud, ต้องต่อ GND ร่วม
- **ก่อนโทษเฟิร์มแวร์เรื่องหลุด ให้ตัดเรื่องไฟออกก่อน** ถ้าจ่ายไฟ UM981 จาก pin 5V
  ของ ESP32 ขณะเสียบ USB ทั้งระบบกินเกิน 500 mA ที่ USB 2.0 จ่ายได้ เจอ
  `Brownout detector was triggered` หรือเห็น `RTK Bridge starting` ซ้ำเรื่อยๆ
  ใน serial = ไฟไม่พอ ไม่ใช่บั๊ก
- แฟลชได้โดยไม่ต้องถอด UM981: bootloader ใช้ GPIO1/3 คนละคู่กับ GPIO16/17
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

### ADB over WiFi ชนกับ AP ของบอร์ด

คอมมีสองเส้นทางที่แยกกัน: **USB → ESP32** (แฟลช + อ่าน serial) และ **ADB → จอ**
(สั่ง force-stop, อ่าน logcat) การแฟลชวิ่งผ่าน USB เท่านั้น ไม่เกี่ยวกับ ADB เลย

แต่ถ้าทดสอบทาง TCP จอจะต้องเข้า AP `RTK-Bridge` (192.168.4.x) ซึ่งทำให้มันหลุดจาก
WiFi บ้าน แล้ว `adb connect` จะหาไม่เจอ เลือกอย่างใดอย่างหนึ่ง:

- ทดสอบทาง Bluetooth: ปล่อยจอกับคอมอยู่ WiFi บ้าน ADB ทำงานปกติ
  (ตั้ง `ENABLE_WIFI_BRIDGE 0` เพื่อตัดตัวแปรทิ้งได้)
- ทดสอบทาง TCP: ให้คอมเข้า AP `RTK-Bridge` ด้วย แล้ว `adb connect 192.168.4.x:5555`
  (AP รับได้ 4 เครื่อง) ระหว่างนั้นคอมจะไม่มีเน็ต

USB console (`Serial`, 115200) แยกขาจาก UART2 ที่คุยกับ UM981 อ่าน log ได้โดยไม่รบกวน
ข้อมูล GNSS

## สถานะ

- ยังไม่เคยคอมไพล์
- `ByteRing` ผ่านเทสต์ host 6 เคสภายใต้ ASan/UBSan
- ยังไม่เคยแฟลชลงบอร์ดจริง
