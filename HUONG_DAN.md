# HƯỚNG DẪN NẠP VÀ CHẠY – NHÀ THÔNG MINH ESP32-S3

## 1. Phần cứng và cấu hình Arduino

- Board: **ESP32S3 Dev Module**
- Flash: **16 MB**
- PSRAM: **OPI PSRAM**
- USB CDC On Boot: **Disabled** nếu dùng cổng UART/COM
- Partition: bản mặc định 16 MB hoặc cấu hình có APP tối thiểu 1,3 MB
- Serial Monitor: **115200 baud**, gửi lệnh với newline

Thư viện đã dùng để build kiểm tra:

- ESP32 Arduino core 3.3.10
- ESP32Servo 3.2.1
- Keypad 3.1.1
- MFRC522 1.4.12
- Adafruit Fingerprint Sensor Library 2.1.4
- DFRobotDFPlayerMini 1.0.6

## 2. Cấu hình bắt buộc trước khi sử dụng

Mở `config.h` và thực hiện các bước sau:

1. Đổi `PASSWORD` từ `CHANGE_ME` thành 4–8 chữ số. Khi chưa đổi, mở cửa bằng mật khẩu và đăng ký vân tay qua Serial đều bị khóa.
2. Quét thẻ để đọc UID trên Serial, sau đó thêm UID thật vào `RFID_UIDS`. Chuỗi rỗng mặc định không cấp quyền cho thẻ nào.
3. Kiểm tra góc servo trước khi lắp cơ khí để tránh làm kẹt cửa hoặc mái che.
4. Sau khi MQ2 ổn định, chỉnh `MQ2_ALARM_V` theo điện áp thực tế trong không khí sạch.

Mặc định an toàn:

- `VOICE_AUTO false`: chỉ thu giọng khi bấm `A`.
- `VOICE_OPENS_DOOR false`: giọng nói chỉ nhận diện/phát lời chào, không mở cửa.
- `ACCESS_REQUIRE_PCF true`: mất PCF8574 thì hệ thống từ chối mở cửa.

Nhận dạng người nói không có khả năng chống phát lại. Chỉ bật `VOICE_OPENS_DOOR true` nếu chấp nhận việc bản ghi âm giọng chủ nhà có thể được dùng để mở cửa.

## 3. Đấu nối microphone

Theo `pins.h`:

- SCK → GPIO4
- WS → GPIO5
- SD → GPIO6
- L/R → GND để dùng kênh trái
- VDD → 3,3 V

Phải giữ `VOICE_GAIN_SHIFT` giống cấu hình lúc thu dữ liệu huấn luyện.

## 4. PCF8574 và relay

Địa chỉ mặc định là `0x27`; đổi thành `0x3F` nếu I²C Scanner báo địa chỉ đó.

Firmware tự thử kết nối lại PCF8574 mỗi 2 giây. Khi mất kết nối, input cũ không được tái sử dụng và quyền mở cửa bị khóa nếu `ACCESS_REQUIRE_PCF` đang bật.

PCF8574 thường khởi động các chân ở mức HIGH. Vì `P_FAN` hiện là relay active-HIGH, relay vẫn có thể nháy rất ngắn trước khi firmware chạy. Với tải quan trọng, cần thêm điện trở/mạch khóa phần cứng hoặc dùng tầng đảo active-LOW; firmware không thể loại bỏ hoàn toàn trạng thái lúc cấp nguồn.

## 5. Thẻ nhớ DFPlayer

Đặt file trong thư mục `/mp3/`:

| File | Nội dung gợi ý |
|---|---|
| `0001.mp3` | Xin chào chủ nhà |
| `0002.mp3` | Cảnh báo, phát hiện người lạ |
| `0003.mp3` | Mời vào |
| `0004.mp3` | Sai, vui lòng thử lại |

Độ dài file phát không nên vượt quá `TRACK_MUTE_MS`, nếu không loa có thể bị microphone thu lại.

## 6. Đăng ký vân tay an toàn

Đăng ký vân tay không còn chặn vòng lặp cảm biến. Trên Serial Monitor:

```text
AUTH 5678
E1
```

Trong đó `5678` là `PASSWORD` đã cấu hình và `E1` đăng ký ID 1. Có thể dùng ID 1–127. Quyền đăng ký hết hạn sau `SERIAL_AUTH_MS`; gõ `CANCEL` để hủy phiên đang thực hiện.

Trong lúc đăng ký, cửa vẫn tự đóng đúng hạn và cảm biến gas/lửa/mưa vẫn tiếp tục được xử lý.

## 7. Khóa chống thử sai

Sau `MAX_FAIL` lần xác thực sai liên tiếp:

- Còi xâm nhập và camera được kích hoạt.
- Tất cả cách mở cửa bị khóa trong `ACCESS_LOCKOUT_MS`.
- Bộ đếm chỉ được xóa khi hết thời gian khóa hoặc sau một lần mở cửa hợp lệ trước khi chạm ngưỡng.

Tất cả deadline dùng phép so sánh an toàn khi `millis()` tràn, nên không còn lỗi báo động giả sau khoảng 24,9 ngày hoặc lỗi lịch chạy tại mốc 49,7 ngày.

## 8. Log khởi động cần kiểm tra

Một lần khởi động tốt phải có các dòng tương tự:

```text
PCF8574 (0x27): OK
DFPlayer: OK
AS608: OK, da luu ... van tay
RC522: OK (0x92)
[GIONG] Tu kiem tra mo hinh: DAT (...)
Giong noi: bam 'A' de noi
```

Nếu self-test voice, I2S, queue hoặc task thất bại, phần nhận dạng giọng nói bị vô hiệu hóa thay vì tiếp tục mở cửa trong trạng thái không tin cậy.

## 9. Build kiểm tra bằng Arduino CLI

```powershell
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi" .
```

Build kiểm tra hiện tại sử dụng khoảng 611 KB flash và 37 KB RAM tĩnh. Các buffer CNN/audio được cấp phát khoảng 438 KiB trong PSRAM lúc chạy.

## 10. Phím điều khiển

- Số + `#`: xác nhận mật khẩu
- `*`: xóa chuỗi mật khẩu đang nhập
- `A`: thu và nhận dạng giọng trong 2 giây
- `D`: đóng cửa ngay

Mô hình hiện không có nhãn nền `nen`. Vì vậy nên giữ `VOICE_AUTO=false` cho đến khi mô hình được huấn luyện lại với dữ liệu nền/tiếng ồn đầy đủ.
