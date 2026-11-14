/*
 * NHA THONG MINH ESP32-S3-N16R8 - NHAN DIEN GIONG CHU NHA
 * ---------------------------------------------------------------
 *  - Giong chu nha  -> loa phat "Xin chao chu nha" + mo cua
 *  - Giong nguoi la -> loa phat "Canh bao nguoi la" + kich camera (P7 PCF8574)
 *  - Keypad / van tay AS608 / the RC522 dung -> servo 2 mo cua 90 do
 *  - Sai 3 lan lien tiep -> coi bao xam nhap + kich camera
 *  - Cua mo -> quat chay, cua dong -> quat tat
 *  - Co mua -> servo 1 quay 90 do che mai, tanh mua -> mo lai
 *  - Khoi/gas (MQ2) hoac lua -> coi keu
 *
 * Keypad: so + '#' = nhap mat khau | '*' = xoa | 'A' = bam roi noi (nhan dien giong)
 *         'D' = dong cua ngay
 * Serial Monitor (115200): go "E1" de dang ky van tay ID 1 (E2, E3...)
 *
 * Tools: ESP32S3 Dev Module, Flash 16MB, PSRAM: OPI PSRAM,
 *        Partition: 16M Flash (3MB APP/9.9MB FATFS) hoac Huge APP
 * Thu vien: ESP32Servo, Keypad (Mark Stanley), MFRC522, Adafruit Fingerprint Sensor Library,
 *           DFRobotDFPlayerMini
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
Servo servoRoof;   // SERVO1 - mai che mua
Servo servoDoor;   // SERVO2 - cua
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, KEYPAD_ROWS, KEYPAD_COLS);
MFRC522 rfid(RFID_SS_PIN, MFRC522::UNUSED_PIN);
HardwareSerial DFSerial(1);
HardwareSerial FPSerial(2);
Adafruit_Fingerprint finger(&FPSerial);
DFRobotDFPlayerMini df;

bool dfOK = false, fpOK = false, pcfOK = false;

// ============================================================ PCF8574
const uint8_t PCF_INPUTS = (1 << P_FLAME) | (1 << P_RAIN) | (1 << P_FP_TCH);
uint8_t pcfOut = PCF_INIT_STATE;
uint8_t pcfIn = 0xFF;

void pcfFlush() {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(pcfOut | PCF_INPUTS);          // chan vao luon giu muc 1
  Wire.endTransmission();
}
void pcfSet(uint8_t bit, bool high) {
  uint8_t old = pcfOut;
  if (high) pcfOut |= (1 << bit); else pcfOut &= ~(1 << bit);
  if (pcfOut != old) pcfFlush();
}
void pcfReadInputs() {
  if (Wire.requestFrom((uint8_t)PCF_ADDR, (uint8_t)1) == 1) pcfIn = Wire.read();
}
inline bool pinIn(uint8_t bit) { return pcfIn & (1 << bit); }

void setLed1(bool on) { pcfSet(P_LED1, !on); }   // LOW = sang
void setLed2(bool on) { pcfSet(P_LED2, !on); }
void setBuzz(bool on) { pcfSet(P_BUZZ, !on); }   // LOW = keu
void setFan(bool on)  { pcfSet(P_FAN, on); }     // HIGH = quat chay

// ============================================================ Trang thai
bool doorOpen = false;     uint32_t doorCloseAt = 0;
bool fanOn = false;        uint32_t fanOffAt = 0;
bool roofClosed = false;   uint32_t rainSince = 0, drySince = 0;
bool gasAlarm = false, fireAlarm = false;
int flameCount = 0;
float mq2V = 0;
int failCount = 0;
uint32_t intruderUntil = 0, camPulseEnd = 0, clickUntil = 0, manualLedUntil = 0;
String pwBuf = "";

// ============================================================ Tien ich
void playTrack(int n) {
  if (!dfOK || n <= 0) return;
  df.playMp3Folder(n);
  voiceMute(TRACK_MUTE_MS);
}

void camTrigger() {
  if (!CAM_TRIGGER_ENABLE) return;
  pcfSet(P_SPARE, false);
  camPulseEnd = millis() + CAM_PULSE_MS;
  Serial.println("[CAM] Kich camera chup anh");
}

void openDoor(const char *why) {
  Serial.printf("[CUA] MO CUA - %s\n", why);
  servoDoor.write(DOOR_OPEN_ANGLE);
  doorOpen = true;
  doorCloseAt = millis() + DOOR_OPEN_MS;
  setLed1(true);
  setFan(true); fanOn = true; fanOffAt = 0;
  failCount = 0;
  voiceMute(1000);
}

void closeDoor() {
  Serial.println("[CUA] Dong cua");
  servoDoor.write(DOOR_CLOSED_ANGLE);
  doorOpen = false;
  setLed1(false);
  fanOffAt = millis() + FAN_AFTER_CLOSE_MS;
  voiceMute(1000);
}

void accessOK(const char *why) {
  playTrack(TRACK_ACCESS_OK);
  openDoor(why);
}

void accessFail(const char *why) {
  failCount++;
  Serial.printf("[CANH BAO] %s sai (%d/%d)\n", why, failCount, MAX_FAIL);
  if (failCount >= MAX_FAIL) {
    Serial.println("[CANH BAO] XAM NHAP! Bat coi + chup anh");
    intruderUntil = millis() + INTRUDER_ALARM_MS;
    playTrack(TRACK_STRANGER);
    camTrigger();
    failCount = 0;
  } else {
    playTrack(TRACK_ACCESS_FAIL);
  }
}

// ============================================================ Keypad
void handleKeypad() {
  char k = keypad.getKey();
  if (!k) return;
  setBuzz(true); clickUntil = millis() + 30;     // tieng bip nho khi bam phim
  voiceMute(300);

  if (k >= '0' && k <= '9') {
    if (pwBuf.length() < 8) pwBuf += k;
    String stars;
    for (unsigned i = 0; i < pwBuf.length(); i++) stars += '*';
    Serial.printf("[PHIM] Mat khau: %s\n", stars.c_str());
  } else if (k == '*') {
    pwBuf = "";
    Serial.println("[PHIM] Xoa mat khau");
  } else if (k == '#') {
    if (pwBuf == PASSWORD) accessOK("mat khau dung");
    else if (pwBuf.length()) accessFail("Mat khau");
    pwBuf = "";
  } else if (k == 'A') {
    Serial.println("[GIONG] Hay noi cau khoa ngay (2 giay)...");
    setLed2(true); manualLedUntil = millis() + 2100;
    delay(150);                                   // cho het tieng bip
    voiceRequestManual();
  } else if (k == 'D') {
    if (doorOpen) closeDoor();
  }
}

// ============================================================ RFID
void handleRFID() {
  static uint32_t next = 0;
  if (millis() < next) return;
  next = millis() + 200;
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
  for (auto u : RFID_UIDS) if (uid.equalsIgnoreCase(u)) ok = true;
  if (ok) accessOK("the RFID hop le"); else accessFail("The RFID");

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  next = millis() + 1500;
}

// ============================================================ Van tay
void handleFingerprint() {
  static uint32_t next = 0;
  if (!fpOK || millis() < next) return;
  if (!pinIn(P_FP_TCH)) return;                   // TCH = HIGH khi co ngon tay
  next = millis() + 300;

  if (finger.getImage() != FINGERPRINT_OK) return;
  if (finger.image2Tz() != FINGERPRINT_OK) { next = millis() + 1000; return; }
  uint8_t r = finger.fingerFastSearch();
  if (r == FINGERPRINT_OK && finger.confidence >= FP_MIN_CONFIDENCE) {
    Serial.printf("[VAN TAY] ID %d, do tin cay %d\n", finger.fingerID, finger.confidence);
    accessOK("van tay hop le");
  } else {
    accessFail("Van tay");
  }
  next = millis() + 2000;                         // tranh doc lai khi tay van dat tren cam bien
}

uint8_t fpGetImageWait() {
  uint8_t p;
  uint32_t t0 = millis();
  while ((p = finger.getImage()) != FINGERPRINT_OK) {
    if (millis() - t0 > 10000) return p;
    delay(50);
  }
  return p;
}

void enrollFinger(uint16_t id) {
  if (!fpOK) { Serial.println("[VAN TAY] Khong ket noi duoc AS608"); return; }
  Serial.printf("[VAN TAY] Dang ky ID %d - dat ngon tay len cam bien...\n", id);
  if (fpGetImageWait() != FINGERPRINT_OK || finger.image2Tz(1) != FINGERPRINT_OK) { Serial.println("  Loi lay anh lan 1"); return; }
  Serial.println("  Nhac ngon tay ra...");
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);
  Serial.println("  Dat lai CUNG ngon tay do...");
  if (fpGetImageWait() != FINGERPRINT_OK || finger.image2Tz(2) != FINGERPRINT_OK) { Serial.println("  Loi lay anh lan 2"); return; }
  if (finger.createModel() != FINGERPRINT_OK) { Serial.println("  2 lan khong khop, thu lai"); return; }
  if (finger.storeModel(id) != FINGERPRINT_OK) { Serial.println("  Loi luu"); return; }
  Serial.printf("  DA LUU van tay ID %d\n", id);
}

void handleSerialCmd() {
  if (!Serial.available()) return;
  String s = Serial.readStringUntil('\n');
  s.trim();
  if (s.length() > 1 && (s[0] == 'E' || s[0] == 'e')) {
    int id = s.substring(1).toInt();
    if (id >= 1 && id <= 127) enrollFinger(id);
  }
}

// ============================================================ Giong noi
void handleVoice() {
  VoiceResult r;
  if (!voiceGetResult(r)) return;

  Serial.printf("[GIONG] (%s, %u ms) ", r.manual ? "phim A" : "tu dong", (unsigned)r.ms);
  int best = 0;
  for (int i = 0; i < r.n; i++) {
    Serial.printf("%s=%.2f  ", voiceLabel(i), r.prob[i]);
    if (r.prob[i] > r.prob[best]) best = i;
  }
  Serial.println();

  int noise = voiceNoiseIndex();
  if (noise >= 0 && best == noise) { Serial.println("  -> tieng on, bo qua"); return; }

  if (r.prob[voiceOwnerIndex()] >= OWNER_THRESHOLD) {
    Serial.println("  -> CHU NHA");
    playTrack(TRACK_OWNER);
    if (VOICE_OPENS_DOOR && !doorOpen) openDoor("giong chu nha");
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
  if (now < next) return;
  next = now + 100;

  pcfReadInputs();

  // --- Lua (LOW = co lua), can 3 lan lien tiep ---
  flameCount = !pinIn(P_FLAME) ? min(flameCount + 1, 5) : 0;
  bool fire = flameCount >= 3;
  if (fire != fireAlarm) { fireAlarm = fire; Serial.println(fire ? "[BAO CHAY] PHAT HIEN LUA!" : "[BAO CHAY] Het lua"); }

  // --- MQ2 ---
  float v = analogReadMilliVolts(MQ2_PIN) / 1000.0f * MQ2_DIV_RATIO;
  mq2V = mq2V == 0 ? v : 0.8f * mq2V + 0.2f * v;
  if (now > MQ2_WARMUP_MS) {
    if (!gasAlarm && mq2V > MQ2_ALARM_V) { gasAlarm = true; Serial.printf("[KHI GAS] CANH BAO! MQ2 = %.2f V\n", mq2V); }
    if (gasAlarm && mq2V < MQ2_ALARM_V - MQ2_HYST_V) { gasAlarm = false; Serial.println("[KHI GAS] Binh thuong"); }
  }

  // --- Mua (LOW = co mua) ---
  bool rain = !pinIn(P_RAIN);
  if (rain) { drySince = 0; if (!rainSince) rainSince = now; }
  else      { rainSince = 0; if (!drySince) drySince = now; }
  if (!roofClosed && rain && now - rainSince >= RAIN_ON_MS) {
    roofClosed = true; servoRoof.write(ROOF_CLOSED_ANGLE); voiceMute(1000);
    Serial.println("[MUA] Co mua -> dong mai che");
  }
  if (roofClosed && !rain && now - drySince >= RAIN_OFF_MS) {
    roofClosed = false; servoRoof.write(ROOF_OPEN_ANGLE); voiceMute(1000);
    Serial.println("[MUA] Tanh mua -> mo mai che");
  }

  if (now - lastPrint > 3000) {
    lastPrint = now;
    Serial.printf("[TT] MQ2=%.2fV%s  Lua=%s  Mua=%s  Cua=%s  Mai=%s  Quat=%s\n",
                  mq2V, now < MQ2_WARMUP_MS ? "(dang u nong)" : "",
                  fireAlarm ? "CO" : "khong", rain ? "CO" : "khong",
                  doorOpen ? "MO" : "dong", roofClosed ? "dong" : "mo", fanOn ? "chay" : "tat");
  }
}

// ============================================================ Dau ra theo thoi gian
void handleOutputs() {
  uint32_t now = millis();

  if (doorOpen && (int32_t)(now - doorCloseAt) >= 0) closeDoor();
  if (!doorOpen && fanOn && (int32_t)(now - fanOffAt) >= 0) { setFan(false); fanOn = false; }

  if (camPulseEnd && (int32_t)(now - camPulseEnd) >= 0) { pcfSet(P_SPARE, true); camPulseEnd = 0; }
  if (manualLedUntil && (int32_t)(now - manualLedUntil) >= 0) { manualLedUntil = 0; }

  bool alarm = fireAlarm || gasAlarm || (int32_t)(intruderUntil - now) > 0;
  if (alarm) {
    bool phase = (now / 250) % 2;                 // coi keu ngat quang
    setBuzz(phase);
    setLed2(phase);
    voiceMute(500);                               // coi keu -> khong nghe giong
  } else {
    setBuzz((int32_t)(clickUntil - now) > 0);
    setLed2(manualLedUntil != 0);
  }
}

// ============================================================ SETUP / LOOP
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n===== NHA THONG MINH ESP32-S3 =====");

  // PCF8574
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.beginTransmission(PCF_ADDR);
  pcfOK = Wire.endTransmission() == 0;
  pcfFlush();
  Serial.printf("PCF8574 (0x%02X): %s\n", PCF_ADDR, pcfOK ? "OK" : "KHONG THAY - doi PCF_ADDR (0x27/0x3F)");

  // Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoRoof.setPeriodHertz(50); servoRoof.attach(SERVO1_PIN, 500, 2400); servoRoof.write(ROOF_OPEN_ANGLE);
  servoDoor.setPeriodHertz(50); servoDoor.attach(SERVO2_PIN, 500, 2400); servoDoor.write(DOOR_CLOSED_ANGLE);

  // DFPlayer
  DFSerial.begin(DF_BAUD, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  dfOK = df.begin(DFSerial, true, true);
  if (dfOK) { df.setTimeOut(500); df.volume(DF_VOLUME); }
  Serial.printf("DFPlayer: %s\n", dfOK ? "OK" : "LOI (kiem tra the SD / day)");

  // Van tay
  FPSerial.begin(FP_BAUD, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(FP_BAUD);
  fpOK = finger.verifyPassword();
  if (fpOK) { finger.getTemplateCount(); Serial.printf("AS608: OK, da luu %d van tay\n", finger.templateCount); }
  else Serial.println("AS608: LOI (kiem tra day TX/RX)");

  // RFID
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522: %s (0x%02X)\n", (ver == 0x91 || ver == 0x92 || ver == 0x88) ? "OK" : "LOI?", ver);

  // MQ2
  analogSetPinAttenuation(MQ2_PIN, ADC_11db);

  // Giong noi (chay tren Core 0)
  if (!voiceBegin()) Serial.println("Giong noi: LOI");
  Serial.printf("Giong noi: %s, nguong chu nha %.2f\n", VOICE_AUTO ? "tu dong nghe" : "bam 'A' de noi", OWNER_THRESHOLD);
  if (voiceNoiseIndex() < 0)
    Serial.println("  Luu y: mo hinh chua co nhan 'nen' -> tieng on lon co the bi bao nham la nguoi la");

  Serial.println("San sang!\n");
}

void loop() {
  handleSerialCmd();
  handleKeypad();
  handleRFID();
  handleFingerprint();
  handleSensors();
  handleVoice();
  handleOutputs();
  delay(5);
}
