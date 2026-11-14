/*
 * pins.h - Khai bao chan ESP32-S3-N16R8 (DevKitC-1)
 * Thiet bi: Keypad 4x4, RC522, AS608, INMP441, MQ2, DFPlayer Mini,
 *           2 servo, mach I2C cua LCD (PCF8574) cho: 2 LED, coi,
 *           cam bien lua, cam bien mua, relay quat, TCH cua AS608
 */
#pragma once

// =====================================================================
// HANG CHAN TRAI (J1) - cac module
// =====================================================================

// --- Mic INMP441 (I2S) ---   L/R -> GND, VDD -> 3.3V
#define I2S_SCK_PIN     4
#define I2S_WS_PIN      5
#define I2S_SD_PIN      6

// --- Cam bien gas MQ2 (analog) ---   AO -> 10k -> GPIO7 -> 15k -> GND
#define MQ2_PIN         7      // ADC1_CH6
#define MQ2_DIV_RATIO   (25.0f / 15.0f)   // V_AO = V_GPIO * 25/15

// --- Servo ---
#define SERVO1_PIN      15
#define SERVO2_PIN      16

// --- DFPlayer Mini (UART1) ---
#define DF_TX_PIN       17     // ESP TX -> DFPlayer RX (qua 1k)
#define DF_RX_PIN       18     // ESP RX <- DFPlayer TX
#define DF_BAUD         9600

// --- Van tay AS608 (UART2) ---
#define FP_RX_PIN       8      // ESP RX <- AS608 TX
#define FP_TX_PIN       3      // ESP TX -> AS608 RX
#define FP_BAUD         57600

// GPIO46: BO TRONG (chan strapping)

// --- RFID RC522 (SPI) ---   RST -> 3.3V (khong dung chan reset)
#define RFID_SS_PIN     9
#define SPI_SCK_PIN     10
#define SPI_MOSI_PIN    11
#define SPI_MISO_PIN    12

// --- I2C (mach I2C cua LCD / PCF8574, cap 3.3V) ---
#define I2C_SDA_PIN     13
#define I2C_SCL_PIN     14

// =====================================================================
// HANG CHAN PHAI (J3) - Keypad 4x4
// =====================================================================
#define KEYPAD_ROWS     4
#define KEYPAD_COLS     4
static byte rowPins[KEYPAD_ROWS] = {1, 2, 42, 41};    // R1..R4
static byte colPins[KEYPAD_COLS] = {40, 39, 47, 21};  // C1..C4
static char keys[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// =====================================================================
// PCF8574 - mach I2C cua LCD (chan tren hang 16 chan -> bit)
// =====================================================================
#define PCF_ADDR        0x27   // hoac 0x3F - kiem tra bang I2C Scanner

#define P_LED1          0      // chan 4  - LOW  = LED 1 sang
#define P_LED2          1      // chan 5  - LOW  = LED 2 sang
#define P_FLAME         2      // chan 6  - LOW  = co lua        (vao)
#define P_FAN           3      // chan 16 - HIGH = relay dong, quat chay (qua transistor)
#define P_RAIN          4      // chan 11 - LOW  = co mua        (vao)
#define P_FP_TCH        5      // chan 12 - HIGH = co ngon tay   (vao)
#define P_BUZZ          6      // chan 13 - LOW  = coi keu
#define P_SPARE         7      // chan 14 - trong

// Trang thai ban dau: tat ca = 1 (LED tat, coi tat, ngo vao san sang), rieng P3 = 0 (quat tat)
#define PCF_INIT_STATE  (0xFF & ~(1 << P_FAN))

// =====================================================================
// Khoi tao mau (dat trong setup())
// =====================================================================
// SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, RFID_SS_PIN);
// MFRC522 rfid(RFID_SS_PIN, MFRC522::UNUSED_PIN);
// Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
// HardwareSerial DFSerial(1);  DFSerial.begin(DF_BAUD, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
// HardwareSerial FPSerial(2);  FPSerial.begin(FP_BAUD, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);  // truoc finger.begin()
// analogSetPinAttenuation(MQ2_PIN, ADC_11db);
// Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, KEYPAD_ROWS, KEYPAD_COLS);
