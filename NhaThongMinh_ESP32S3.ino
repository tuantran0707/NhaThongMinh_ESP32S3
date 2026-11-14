/*
 * NHA THONG MINH ESP32-S3-N16R8 - NHAN DIEN GIONG CHU NHA
 * Keypad: so + '#' = mat khau | '*' = xoa | 'A' = thu giong | 'D' = dong cua
 * Serial: AUTH <mat_khau>, sau do E1/E2/... de dang ky van tay; CANCEL de huy.
 */
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <ESP32Servo.h>
#include <Keypad.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <DFRobotDFPlayerMini.h>
#include "pins.h"
#include "config.h"
#include "voice.h"

// ============================================================ Doi tuong
Servo servoRoof;
Servo servoDoor;
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, KEYPAD_ROWS, KEYPAD_COLS);
MFRC522 rfid(RFID_SS_PIN, MFRC522::UNUSED_PIN);
HardwareSerial DFSerial(1);
HardwareSerial FPSerial(2);
Adafruit_Fingerprint finger(&FPSerial);
DFRobotDFPlayerMini df;

bool dfOK = false, fpOK = false, pcfOK = false, voiceOK = false;

// ============================================================ Thoi gian an toan khi millis() tran
static inline bool deadlinePending(uint32_t now, uint32_t deadline) {
  return deadline != 0 && (int32_t)(deadline - now) > 0;
}
static inline bool deadlineReached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && (int32_t)(now - deadline) >= 0;
}
static inline bool timeBefore(uint32_t now, uint32_t target) {
  return (int32_t)(now - target) < 0;
}

// ============================================================ PCF8574
const uint8_t PCF_INPUTS = (1 << P_FLAME) | (1 << P_RAIN) | (1 << P_FP_TCH);
uint8_t pcfOut = PCF_INIT_STATE;
uint8_t pcfIn = 0xFF;
uint32_t pcfRetryAt = 0;

bool pcfWriteState() {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(pcfOut | PCF_INPUTS);
  return Wire.endTransmission() == 0;
}

void pcfMarkFailed(const char *operation) {
  if (pcfOK) Serial.printf("[PCF8574] Mat ket noi khi %s\n", operation);
  pcfOK = false;
  pcfIn = 0xFF;                            // khong dung gia tri input cu
  pcfRetryAt = millis() + 2000;
}

bool pcfConnect() {
  Wire.beginTransmission(PCF_ADDR);
  if (Wire.endTransmission() != 0) return false;
  if (!pcfWriteState()) return false;
  pcfOK = true;
  pcfRetryAt = 0;
  return true;
}

void pcfMaintain() {
  if (pcfOK) return;
  uint32_t now = millis();
  if (pcfRetryAt && timeBefore(now, pcfRetryAt)) return;
  if (pcfConnect()) Serial.println("[PCF8574] Da ket noi lai");
  else pcfRetryAt = now + 2000;
}

bool pcfSet(uint8_t bit, bool high) {
  uint8_t old = pcfOut;
  if (high) pcfOut |= (1 << bit); else pcfOut &= ~(1 << bit);
  if (pcfOut == old) return pcfOK;
  if (!pcfOK) return false;
  if (!pcfWriteState()) {
    pcfMarkFailed("ghi output");
    return false;
  }
  return true;
}

bool pcfReadInputs() {
  if (!pcfOK) return false;
  if (Wire.requestFrom((uint8_t)PCF_ADDR, (uint8_t)1) != 1) {
    pcfMarkFailed("doc input");
    return false;
  }
  pcfIn = Wire.read();
  return true;
}

inline bool pinIn(uint8_t bit) { return pcfIn & (1 << bit); }
void setLed1(bool on) { pcfSet(P_LED1, !on); }
void setLed2(bool on) { pcfSet(P_LED2, !on); }
void setBuzz(bool on) { pcfSet(P_BUZZ, !on); }
void setFan(bool on)  { pcfSet(P_FAN, on); }

// ============================================================ Trang thai
bool doorOpen = false;     uint32_t doorCloseAt = 0;
bool fanOn = false;        uint32_t fanOffAt = 0;
bool roofClosed = false;   uint32_t rainSince = 0, drySince = 0;
bool rainDetected = false, mq2Warmed = false;
bool gasAlarm = false, fireAlarm = false;
int flameCount = 0;
float mq2V = 0;
int failCount = 0;
uint32_t intruderUntil = 0, lockoutUntil = 0, serialAuthUntil = 0;
uint32_t camPulseEnd = 0, clickUntil = 0, manualLedUntil = 0;
String pwBuf;

enum EnrollState : uint8_t { ENROLL_IDLE, ENROLL_FIRST, ENROLL_REMOVE, ENROLL_SECOND };
EnrollState enrollState = ENROLL_IDLE;
uint16_t enrollId = 0;
uint32_t enrollDeadline = 0, enrollPollAt = 0;

// ============================================================ Cau hinh / bao mat
bool passwordConfigured() {
  size_t n = strlen(PASSWORD);
  if (n < 4 || n > 8) return false;
  for (size_t i = 0; i < n; i++) if (PASSWORD[i] < '0' || PASSWORD[i] > '9') return false;
  return true;
}

bool accessLocked() {
  uint32_t now = millis();
  if (lockoutUntil && !deadlinePending(now, lockoutUntil)) {
    lockoutUntil = 0;
    failCount = 0;
    Serial.println("[BAO MAT] Da het thoi gian khoa truy cap");
  }
  return deadlinePending(now, lockoutUntil);
}

bool canGrantAccess(const char *why) {
  if (accessLocked()) {
    Serial.printf("[BAO MAT] Dang khoa, tu choi %s\n", why);
    return false;
  }
  if (ACCESS_REQUIRE_PCF && !pcfOK) {
    Serial.printf("[BAO MAT] PCF8574 loi, tu choi %s\n", why);
    return false;
  }
  return true;
}

// ============================================================ Tien ich / cua
void playTrack(int n) {
  if (!dfOK || n <= 0) return;
  df.playMp3Folder(n);
  if (voiceOK) voiceMute(TRACK_MUTE_MS);
}

void camTrigger() {
  if (!CAM_TRIGGER_ENABLE) return;
  if (!pcfOK || !pcfSet(P_SPARE, false)) {
    Serial.println("[CAM] LOI: khong kich duoc camera vi PCF8574 mat ket noi");
    return;
  }
  camPulseEnd = millis() + CAM_PULSE_MS;
  Serial.println("[CAM] Kich camera chup anh");
}

void openDoor(const char *why) {
  Serial.printf("[CUA] MO CUA - %s\n", why);
  servoDoor.write(DOOR_OPEN_ANGLE);
  doorOpen = true;
  doorCloseAt = millis() + DOOR_OPEN_MS;
  setLed1(true);
  setFan(true);
  fanOn = true;
  fanOffAt = 0;
  failCount = 0;
  if (voiceOK) voiceMute(1000);
}

void closeDoor() {
  Serial.println("[CUA] Dong cua");
  servoDoor.write(DOOR_CLOSED_ANGLE);
  doorOpen = false;
  doorCloseAt = 0;
  setLed1(false);
  if (FAN_AFTER_CLOSE_MS == 0) {
    setFan(false);
    fanOn = false;
    fanOffAt = 0;
  } else {
    fanOffAt = millis() + FAN_AFTER_CLOSE_MS;
  }
  if (voiceOK) voiceMute(1000);
}

bool grantAccess(const char *why, bool playAccessTrack) {
  if (!canGrantAccess(why)) return false;
  if (playAccessTrack) playTrack(TRACK_ACCESS_OK);
  openDoor(why);
  return true;
}

void accessOK(const char *why) { grantAccess(why, true); }

void accessFail(const char *why) {
  if (accessLocked()) return;
  failCount++;
  Serial.printf("[CANH BAO] %s sai (%d/%d)\n", why, failCount, MAX_FAIL);
  if (failCount >= MAX_FAIL) {
    uint32_t now = millis();
    intruderUntil = now + INTRUDER_ALARM_MS;
    lockoutUntil = now + ACCESS_LOCKOUT_MS;
    Serial.printf("[CANH BAO] XAM NHAP! Khoa truy cap %u giay\n", (unsigned)(ACCESS_LOCKOUT_MS / 1000));
    playTrack(TRACK_STRANGER);
    camTrigger();
  } else {
    playTrack(TRACK_ACCESS_FAIL);
  }
}

// ============================================================ Keypad
void handleKeypad() {
  char k = keypad.getKey();
  if (!k) return;
  setBuzz(true);
  clickUntil = millis() + 30;
  if (voiceOK) voiceMute(300);

  if (k >= '0' && k <= '9') {
    if (pwBuf.length() < 8) pwBuf += k;
    Serial.printf("[PHIM] Mat khau: %.*s\n", (int)pwBuf.length(), "********");
  } else if (k == '*') {
    pwBuf = "";
    Serial.println("[PHIM] Xoa mat khau");
  } else if (k == '#') {
    if (!passwordConfigured()) Serial.println("[BAO MAT] PASSWORD chua duoc cau hinh trong config.h");
    else if (pwBuf == PASSWORD) accessOK("mat khau dung");
    else if (pwBuf.length()) accessFail("Mat khau");
    pwBuf = "";
  } else if (k == 'A') {
    if (!voiceOK) {
      Serial.println("[GIONG] Khong san sang; kiem tra log khoi dong");
    } else {
      Serial.println("[GIONG] Hay noi cau khoa ngay (2 giay)...");
      setLed2(true);
      manualLedUntil = millis() + 2100;
      voiceMute(180);                            // bo tieng bip, task van nhan manual request
      voiceRequestManual();
    }
  } else if (k == 'D' && doorOpen) {
    closeDoor();
  }
}

// ============================================================ RFID
void handleRFID() {
  static uint32_t next = 0;
  uint32_t now = millis();
  if (next && timeBefore(now, next)) return;
  next = now + 200;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i) uid += ' ';
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  Serial.printf("[RFID] UID: %s\n", uid.c_str());

  bool ok = false;
  for (const char *allowed : RFID_UIDS)
    if (allowed && allowed[0] && uid.equalsIgnoreCase(allowed)) ok = true;
  if (ok) accessOK("the RFID hop le"); else accessFail("The RFID");

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  next = millis() + 1500;
}

// ============================================================ Van tay khong blocking
void finishEnroll(bool ok, const char *message) {
  Serial.printf("[VAN TAY] %s%s\n", ok ? "THANH CONG: " : "THAT BAI: ", message);
  enrollState = ENROLL_IDLE;
  enrollId = 0;
  enrollDeadline = 0;
  enrollPollAt = 0;
}

void startEnrollFinger(uint16_t id) {
  if (!fpOK) { Serial.println("[VAN TAY] Khong ket noi duoc AS608"); return; }
  if (enrollState != ENROLL_IDLE) { Serial.println("[VAN TAY] Dang co phien dang ky; go CANCEL de huy"); return; }
  enrollId = id;
  enrollState = ENROLL_FIRST;
  enrollDeadline = millis() + FP_ENROLL_TIMEOUT_MS;
  enrollPollAt = 0;
  Serial.printf("[VAN TAY] Dang ky ID %u - dat ngon tay len cam bien...\n", id);
}

void handleFingerEnrollment() {
  if (enrollState == ENROLL_IDLE) return;
  uint32_t now = millis();
  if (deadlineReached(now, enrollDeadline)) {
    finishEnroll(false, "het thoi gian cho");
    return;
  }
  if (enrollPollAt && timeBefore(now, enrollPollAt)) return;
  enrollPollAt = now + 100;

  uint8_t p = finger.getImage();
  if (enrollState == ENROLL_REMOVE) {
    if (p == FINGERPRINT_NOFINGER) {
      enrollState = ENROLL_SECOND;
      enrollDeadline = now + FP_ENROLL_TIMEOUT_MS;
      Serial.println("[VAN TAY] Dat lai CUNG ngon tay do...");
    } else if (p != FINGERPRINT_OK) {
      finishEnroll(false, "loi cam bien khi cho nhac tay");
    }
    return;
  }

  if (p == FINGERPRINT_NOFINGER) return;
  if (p != FINGERPRINT_OK) {
    finishEnroll(false, "khong lay duoc anh");
    return;
  }

  if (enrollState == ENROLL_FIRST) {
    if (finger.image2Tz(1) != FINGERPRINT_OK) {
      finishEnroll(false, "loi chuyen anh lan 1");
      return;
    }
    enrollState = ENROLL_REMOVE;
    enrollDeadline = now + FP_ENROLL_TIMEOUT_MS;
    Serial.println("[VAN TAY] Da lay anh lan 1 - nhac ngon tay ra...");
    return;
  }

  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    finishEnroll(false, "loi chuyen anh lan 2");
    return;
  }
  if (finger.createModel() != FINGERPRINT_OK) {
    finishEnroll(false, "hai lan khong khop");
    return;
  }
  if (finger.storeModel(enrollId) != FINGERPRINT_OK) {
    finishEnroll(false, "khong luu duoc mau");
    return;
  }
  char message[32];
  snprintf(message, sizeof(message), "da luu ID %u", enrollId);
  finishEnroll(true, message);
}

void handleFingerprint() {
  static uint32_t next = 0;
  if (!fpOK || !pcfOK || enrollState != ENROLL_IDLE || accessLocked()) return;
  uint32_t now = millis();
  if ((next && timeBefore(now, next)) || !pinIn(P_FP_TCH)) return;
  next = now + 300;

  if (finger.getImage() != FINGERPRINT_OK) return;
  if (finger.image2Tz() != FINGERPRINT_OK) { next = millis() + 1000; return; }
  uint8_t result = finger.fingerFastSearch();
  if (result == FINGERPRINT_OK && finger.confidence >= FP_MIN_CONFIDENCE) {
    Serial.printf("[VAN TAY] ID %u, do tin cay %u\n", finger.fingerID, finger.confidence);
    accessOK("van tay hop le");
  } else {
    accessFail("Van tay");
  }
  next = millis() + 2000;
}

// ============================================================ Lenh Serial khong blocking
void processSerialCommand(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  size_t n = strlen(line);
  while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
  if (!n) return;

  if ((line[0] == 'A' || line[0] == 'a') &&
      (line[1] == 'U' || line[1] == 'u') &&
      (line[2] == 'T' || line[2] == 't') &&
      (line[3] == 'H' || line[3] == 'h') && line[4] == ' ') {
    if (!passwordConfigured()) {
      Serial.println("[BAO MAT] Hay dat PASSWORD 4..8 chu so truoc");
    } else if (strcmp(line + 5, PASSWORD) == 0) {
      serialAuthUntil = millis() + SERIAL_AUTH_MS;
      Serial.println("[BAO MAT] Serial da xac thuc; co the dung E1..E127");
    } else {
      serialAuthUntil = 0;
      Serial.println("[BAO MAT] Xac thuc Serial that bai");
    }
    return;
  }

  if ((line[0] == 'C' || line[0] == 'c') &&
      (line[1] == 'A' || line[1] == 'a') &&
      (line[2] == 'N' || line[2] == 'n') &&
      (line[3] == 'C' || line[3] == 'c') &&
      (line[4] == 'E' || line[4] == 'e') &&
      (line[5] == 'L' || line[5] == 'l') && line[6] == 0) {
    if (enrollState != ENROLL_IDLE) finishEnroll(false, "nguoi dung huy");
    return;
  }

  if ((line[0] == 'E' || line[0] == 'e') && line[1]) {
    if (!deadlinePending(millis(), serialAuthUntil)) {
      serialAuthUntil = 0;
      Serial.println("[BAO MAT] Tu choi. Go AUTH <PASSWORD> truoc khi dang ky van tay");
      return;
    }
    char *end = nullptr;
    long id = strtol(line + 1, &end, 10);
    if (end && *end == 0 && id >= 1 && id <= 127) startEnrollFinger((uint16_t)id);
    else Serial.println("[VAN TAY] ID phai tu 1 den 127");
    return;
  }
  Serial.println("[SERIAL] Lenh: AUTH <PASSWORD> | E1..E127 | CANCEL");
}

void handleSerialCmd() {
  static char buffer[48];
  static uint8_t len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) Serial.println("[SERIAL] Lenh qua dai, da bo qua");
      else { buffer[len] = 0; processSerialCommand(buffer); }
      len = 0;
      overflow = false;
    } else if (!overflow) {
      if (len < sizeof(buffer) - 1) buffer[len++] = c;
      else overflow = true;
    }
  }
}

// ============================================================ Giong noi
void handleVoice() {
  if (!voiceOK) return;
  VoiceResult result;
  if (!voiceGetResult(result)) return;

  Serial.printf("[GIONG] (%s, %u ms) ", result.manual ? "phim A" : "tu dong", (unsigned)result.ms);
  int best = 0;
  for (int i = 0; i < result.n; i++) {
    Serial.printf("%s=%.2f  ", voiceLabel(i), result.prob[i]);
    if (result.prob[i] > result.prob[best]) best = i;
  }
  Serial.println();

  int noise = voiceNoiseIndex();
  if (noise >= 0 && best == noise) { Serial.println("  -> tieng on, bo qua"); return; }
  int owner = voiceOwnerIndex();
  if (owner < 0 || owner >= result.n) {
    Serial.println("[GIONG] LOI: chi so nhan chu nha khong hop le");
    voiceOK = false;
    return;
  }

  if (result.prob[owner] >= OWNER_THRESHOLD) {
    Serial.println("  -> CHU NHA");
    playTrack(TRACK_OWNER);
    if (VOICE_OPENS_DOOR && !doorOpen) grantAccess("giong chu nha", false);
  } else {
    Serial.println("  -> NGUOI LA");
    playTrack(TRACK_STRANGER);
    camTrigger();
  }
}

// ============================================================ Cam bien
void handleSensors() {
  static uint32_t next = 0, lastPrint = 0;
  uint32_t now = millis();
  if (next && timeBefore(now, next)) return;
  next = now + 100;

  if (pcfReadInputs()) {
    flameCount = !pinIn(P_FLAME) ? min(flameCount + 1, 5) : 0;
    bool fire = flameCount >= 3;
    if (fire != fireAlarm) {
      fireAlarm = fire;
      Serial.println(fire ? "[BAO CHAY] PHAT HIEN LUA!" : "[BAO CHAY] Het lua");
    }

    rainDetected = !pinIn(P_RAIN);
    if (rainDetected) { drySince = 0; if (!rainSince) rainSince = now; }
    else              { rainSince = 0; if (!drySince) drySince = now; }
    if (!roofClosed && rainDetected && now - rainSince >= RAIN_ON_MS) {
      roofClosed = true;
      servoRoof.write(ROOF_CLOSED_ANGLE);
      if (voiceOK) voiceMute(1000);
      Serial.println("[MUA] Co mua -> dong mai che");
    }
    if (roofClosed && !rainDetected && now - drySince >= RAIN_OFF_MS) {
      roofClosed = false;
      servoRoof.write(ROOF_OPEN_ANGLE);
      if (voiceOK) voiceMute(1000);
      Serial.println("[MUA] Tanh mua -> mo mai che");
    }
  }

  float v = analogReadMilliVolts(MQ2_PIN) / 1000.0f * MQ2_DIV_RATIO;
  mq2V = mq2V == 0 ? v : 0.8f * mq2V + 0.2f * v;
  if (!mq2Warmed && now >= MQ2_WARMUP_MS) mq2Warmed = true;
  if (mq2Warmed) {
    if (!gasAlarm && mq2V > MQ2_ALARM_V) {
      gasAlarm = true;
      Serial.printf("[KHI GAS] CANH BAO! MQ2 = %.2f V\n", mq2V);
    }
    if (gasAlarm && mq2V < MQ2_ALARM_V - MQ2_HYST_V) {
      gasAlarm = false;
      Serial.println("[KHI GAS] Binh thuong");
    }
  }

  if (now - lastPrint >= 3000) {
    lastPrint = now;
    Serial.printf("[TT] PCF=%s MQ2=%.2fV%s Lua=%s Mua=%s Cua=%s Mai=%s Quat=%s\n",
                  pcfOK ? "OK" : "LOI", mq2V, mq2Warmed ? "" : "(dang u nong)",
                  fireAlarm ? "CO" : "khong", rainDetected ? "CO" : "khong",
                  doorOpen ? "MO" : "dong", roofClosed ? "dong" : "mo", fanOn ? "chay" : "tat");
  }
}

// ============================================================ Dau ra theo thoi gian
void handleOutputs() {
  uint32_t now = millis();
  accessLocked();                                  // dong thoi xoa lockout da het han

  if (deadlineReached(now, doorCloseAt)) closeDoor();
  if (!doorOpen && fanOn && deadlineReached(now, fanOffAt)) {
    setFan(false);
    fanOn = false;
    fanOffAt = 0;
  }
  if (deadlineReached(now, camPulseEnd)) {
    pcfSet(P_SPARE, true);
    camPulseEnd = 0;
  }
  if (deadlineReached(now, manualLedUntil)) manualLedUntil = 0;
  if (intruderUntil && !deadlinePending(now, intruderUntil)) intruderUntil = 0;
  if (clickUntil && !deadlinePending(now, clickUntil)) clickUntil = 0;
  if (serialAuthUntil && !deadlinePending(now, serialAuthUntil)) serialAuthUntil = 0;

  bool alarm = fireAlarm || gasAlarm || deadlinePending(now, intruderUntil);
  if (alarm) {
    bool phase = (now / 250) % 2;
    setBuzz(phase);
    setLed2(phase);
    if (voiceOK) voiceMute(500);
  } else {
    setBuzz(deadlinePending(now, clickUntil));
    setLed2(manualLedUntil != 0);
  }
}

// ============================================================ SETUP / LOOP
void setup() {
  // Khoi tao PCF som nhat co the de giam xung HIGH tren relay active-HIGH luc boot.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  pcfOK = pcfConnect();

  Serial.begin(115200);
  delay(300);
  Serial.println("\n===== NHA THONG MINH ESP32-S3 =====");
  Serial.printf("PCF8574 (0x%02X): %s\n", PCF_ADDR, pcfOK ? "OK" : "KHONG THAY - dang thu ket noi lai");
  if (!pcfOK) pcfRetryAt = millis() + 2000;

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoRoof.setPeriodHertz(50);
  servoRoof.attach(SERVO1_PIN, 500, 2400);
  servoRoof.write(ROOF_OPEN_ANGLE);
  servoDoor.setPeriodHertz(50);
  servoDoor.attach(SERVO2_PIN, 500, 2400);
  servoDoor.write(DOOR_CLOSED_ANGLE);

  DFSerial.begin(DF_BAUD, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  dfOK = df.begin(DFSerial, true, true);
  if (dfOK) { df.setTimeOut(500); df.volume(DF_VOLUME); }
  Serial.printf("DFPlayer: %s\n", dfOK ? "OK" : "LOI (kiem tra the SD / day)");

  FPSerial.begin(FP_BAUD, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(FP_BAUD);
  fpOK = finger.verifyPassword();
  if (fpOK) {
    finger.getTemplateCount();
    Serial.printf("AS608: OK, da luu %u van tay\n", finger.templateCount);
  } else {
    Serial.println("AS608: LOI (kiem tra day TX/RX)");
  }

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522: %s (0x%02X)\n", (version == 0x91 || version == 0x92 || version == 0x88) ? "OK" : "LOI?", version);

  analogSetPinAttenuation(MQ2_PIN, ADC_11db);

  voiceOK = voiceBegin();
  Serial.printf("Giong noi: %s\n", voiceOK ? (VOICE_AUTO ? "tu dong nghe" : "bam 'A' de noi") : "LOI - da vo hieu hoa");
  if (voiceOK && voiceNoiseIndex() < 0)
    Serial.println("  Luu y: mo hinh chua co nhan 'nen'; nen giu VOICE_AUTO=false");
  if (VOICE_OPENS_DOOR)
    Serial.println("  CANH BAO: mo cua bang giong co the bi tan cong bang ban ghi am");

  if (!passwordConfigured())
    Serial.println("[BAO MAT] PASSWORD chua hop le. Hay dat 4..8 chu so trong config.h");
  bool hasRFID = false;
  for (const char *allowed : RFID_UIDS) if (allowed && allowed[0]) hasRFID = true;
  if (!hasRFID) Serial.println("[BAO MAT] Chua co UID RFID nao duoc phep");

  Serial.println("San sang! Lenh Serial: AUTH <PASSWORD>, E1..E127, CANCEL\n");
}

void loop() {
  pcfMaintain();
  handleSerialCmd();
  handleKeypad();
  handleRFID();
  handleFingerEnrollment();
  handleFingerprint();
  handleSensors();
  handleVoice();
  handleOutputs();
  delay(5);
}
