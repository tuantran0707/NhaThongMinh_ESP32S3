# HƯỚNG DẪN NẠP VÀ CHẠY – NHÀ THÔNG MINH ESP32-S3

## 1. Các file trong thư mục (giữ nguyên tất cả trong cùng 1 thư mục)
| File | Nội dung |
|---|---|
| `NhaThongMinh_ESP32S3.ino` | Chương trình chính |
| `config.h` | **Cài đặt của bạn**: mật khẩu, UID thẻ, ngưỡng, góc servo, số bài MP3 |
| `pins.h` | Sơ đồ chân (file của bạn, giữ nguyên) |
| `voice.cpp`, `voice.h`, `voice_ai.h` | Nhận diện giọng nói (chạy trên Core 0) |
| `nn_weights.h` | Trọng số mạng nơ-ron, lấy từ mô hình int8 trong file zip của bạn |
| `feature_config.h`, `test_vector.h` | Lấy nguyên từ file zip Colab của bạn |

## 2. Thư viện cần cài (Tools → Manage Libraries)
ESP32Servo · Keypad (Mark Stanley) · MFRC522 (GithubCommunity) · Adafruit Fingerprint Sensor Library · DFRobotDFPlayerMini

## 3. Tools
Board **ESP32S3 Dev Module** · Flash **16MB** · PSRAM **OPI PSRAM** · USB CDC On Boot **Disabled** (cắm cổng COM) · Partition: Default hoặc 16M đều được (code ~610 KB)

## 4. Lưu ý đấu mic theo pins.h
`pins.h` dùng **SCK → GPIO4, WS → GPIO5, SD → GPIO6** (khác code thu âm cũ WS5/SCK6/SD7).
→ Đấu lại mic theo pins.h. Dữ liệu đã thu **vẫn dùng được**, chỉ cần giữ đúng mic, vị trí đặt mic và `VOICE_GAIN_SHIFT = 14`.

## 5. Thẻ SD cho DFPlayer – thư mục `/mp3/`
| File | Nội dung gợi ý |
|---|---|
| 0001.mp3 | "Xin chào chủ nhà" |
| 0002.mp3 | "Cảnh báo, phát hiện người lạ" |
| 0003.mp3 | "Mời vào" (mật khẩu / thẻ / vân tay đúng) |
| 0004.mp3 | "Sai, vui lòng thử lại" |

## 6. Lần chạy đầu – mở Serial Monitor 115200
Phải thấy:
```
PCF8574 (0x27): OK
DFPlayer: OK
AS608: OK, da luu 0 van tay
RC522: OK (0x92)
[GIONG] Tu kiem tra mo hinh: DAT  (sai lech 0.00x, ... ms)
```
- `PCF8574 KHONG THAY` → đổi `PCF_ADDR` thành `0x3F` trong pins.h.
- `Tu kiem tra mo hinh: LOI` → báo lại để kiểm tra.

## 7. Cài đặt ban đầu
1. **Vân tay:** Serial Monitor gõ `E1` + Enter → đặt ngón tay 2 lần theo hướng dẫn → lưu ID 1. Thêm ngón khác: `E2`, `E3`...
2. **Thẻ RFID:** quẹt thẻ → Serial in `[RFID] UID: xx xx xx xx` → chép vào `RFID_UIDS` trong `config.h` → nạp lại.
3. **Mật khẩu:** sửa `PASSWORD` trong `config.h` (mặc định `1234`).
4. **MQ2:** chờ ủ nóng 1 phút, xem dòng `[TT] MQ2=...V` lúc không khí sạch, chỉnh `MQ2_ALARM_V` cao hơn giá trị đó khoảng 0,5–1 V.

## 8. Chức năng
| Sự kiện | Hành động |
|---|---|
| Giọng chủ (điểm ≥ 0,85) | Loa phát 0001 + mở cửa |
| Giọng người lạ | Loa phát 0002 + kéo chân P7 PCF8574 xuống LOW 300 ms (kích camera) |
| Mật khẩu `#` / thẻ / vân tay đúng | Loa 0003, servo 2 quay 90° mở cửa, tự đóng sau 8 s |
| Sai 3 lần liên tiếp | Còi 5 s + loa 0002 + kích camera |
| Cửa mở | Quạt chạy; cửa đóng → quạt tắt |
| Có mưa ≥ 2 s | Servo 1 quay 90° che mái; tạnh 15 s → mở lại |
| Khói/gas hoặc lửa | Còi kêu ngắt quãng + LED2 nháy |

**Phím:** số + `#` = nhập mật khẩu · `*` = xóa · `A` = bấm rồi nói câu khóa · `D` = đóng cửa ngay

## 9. Chỉnh nhận diện giọng (trong config.h)
- Hay **bắt nhầm tiếng ồn** thành người lạ → tăng `VAD_MIN_RMS` (600 → 1000) hoặc đặt `VOICE_AUTO false` để chỉ nhận diện khi bấm `A`.
- **Chủ hay bị từ chối** → giảm `OWNER_THRESHOLD` (0.85 → 0.75).
- **Người lạ lọt qua** → tăng `OWNER_THRESHOLD` (0.9).
- Serial in điểm mỗi lần nhận diện, ví dụ `chu_nha=0.97 ng_la_1=0.02 ng_la_2=0.01` → dùng để chọn ngưỡng.
