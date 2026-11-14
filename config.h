/*
 * config.h - CAI DAT CUA NGUOI DUNG (chi can sua file nay)
 */
#pragma once

// ===================== MAT KHAU / THE / VAN TAY =====================
#define PASSWORD            "CHANGE_ME" // DOI thanh 4..8 chu so; nhap tren keypad roi bam '#'
// UID the RFID duoc phep mo cua. Quet the -> Serial Monitor in UID -> chep vao day
inline constexpr const char *RFID_UIDS[] = {
  "",                                    // thay bang UID that, vi du "AA BB CC DD"
};
#define FP_MIN_CONFIDENCE   50         // do tin cay toi thieu cua van tay
#define MAX_FAIL            3          // sai lien tiep bao nhieu lan thi bao dong xam nhap
#define INTRUDER_ALARM_MS   5000       // thoi gian coi keu khi bi xam nhap
#define ACCESS_LOCKOUT_MS   60000      // khoa cac cach mo cua sau MAX_FAIL
#define SERIAL_AUTH_MS      60000      // AUTH <PASSWORD> cho phep dang ky van tay trong 60 s
#define FP_ENROLL_TIMEOUT_MS 15000     // timeout cho tung buoc dang ky van tay
#define ACCESS_REQUIRE_PCF  true       // PCF8574 loi -> khong mo cua (fail closed)

// ===================== GIONG NOI =====================
#define VOICE_GAIN_SHIFT    14         // PHAI GIONG GAIN_SHIFT luc thu am mau!
#define OWNER_THRESHOLD     0.85f      // diem chu_nha >= nguong nay -> la chu nha
#define VOICE_OPENS_DOOR    false      // true co nguy co bi mo bang ban ghi am (replay)
#define VOICE_AUTO          false      // an toan hon: chi nghe khi bam phim 'A'
#define VAD_MIN_RMS         600        // muc am toi thieu coi la co nguoi noi (tang neu hay bat nham)
#define VAD_RATIO           4.0f       // to hon tieng nen bao nhieu lan
#define VAD_MIN_SPEECH_MS   300        // phai noi lien tuc it nhat bay nhieu ms
#define VOICE_PREROLL_MS    400        // lay them doan truoc khi bat dau noi
#define VOICE_COOLDOWN_MS   2000       // nghi sau moi lan nhan dien

// ===================== CUA / MAI CHE / QUAT =====================
#define DOOR_CLOSED_ANGLE   0          // servo 2 (cua)
#define DOOR_OPEN_ANGLE     90
#define DOOR_OPEN_MS        8000       // cua tu dong dong sau bay nhieu ms (phim 'D' = dong ngay)
#define ROOF_OPEN_ANGLE     0          // servo 1 (mai che mua)
#define ROOF_CLOSED_ANGLE   90
#define RAIN_ON_MS          2000       // mua lien tuc 2 s -> dong mai che
#define RAIN_OFF_MS         15000      // tanh mua 15 s -> mo mai che
#define FAN_AFTER_CLOSE_MS  0          // quat chay them sau khi dong cua (0 = tat ngay)

// ===================== BAO CHAY / KHI GAS =====================
#define MQ2_ALARM_V         1.8f       // dien ap AO cua MQ2 (V) vuot nguong -> bao dong
#define MQ2_HYST_V          0.2f
#define MQ2_WARMUP_MS       60000      // MQ2 can u nong ~1 phut sau khi cap dien

// ===================== MP3 (the SD: /mp3/0001.mp3 ...) =====================
#define DF_VOLUME           25         // 0..30
#define TRACK_OWNER         1          // "Xin chao chu nha"
#define TRACK_STRANGER      2          // "Canh bao, nguoi la"
#define TRACK_ACCESS_OK     3          // "Moi vao"            (0 = khong phat)
#define TRACK_ACCESS_FAIL   4          // "Sai, vui long thu lai" (0 = khong phat)
#define TRACK_MUTE_MS       3000       // tat mic trong luc loa dang phat

// ===================== CAMERA =====================
#define CAM_TRIGGER_ENABLE  true       // nguoi la -> keo chan P7 (P_SPARE) cua PCF8574 xuong LOW 300 ms
#define CAM_PULSE_MS        300
