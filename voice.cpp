/*
 * voice.cpp - Doc mic INMP441, phat hien tieng noi, chay mo hinh nhan dien giong chu nha
 * Xu ly am thanh GIONG HET code thu am (thu_am_inmp441.ino): dich bit GAIN_SHIFT + loc chan DC.
 */
#include <Arduino.h>
#include <driver/i2s.h>
#include "pins.h"
#include "config.h"
#include "voice.h"
#include "voice_ai.h"
#include "test_vector.h"

#define I2S_PORT   I2S_NUM_0
#define CHUNK      320                    // 20 ms
#define RING_LEN   65536                  // ~4 giay, luy thua cua 2
#define RING_MASK  (RING_LEN - 1)

static QueueHandle_t resultQ;
static volatile uint32_t muteUntil = 0;
static volatile bool manualReq = false;
static bool selfTestOK = false;
static int noiseIdx = -1;

static int16_t *ring;
static int16_t *clip;
static float *bufA, *bufB;
static uint32_t wpos = 0;                 // tong so mau da ghi (vi tri ghi trong ring)
static int32_t raw[CHUNK];
static float prevX = 0, prevY = 0;

static void i2sInit() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = FE_SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_SCK_PIN;
  pins.ws_io_num = I2S_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_SD_PIN;

  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
}

// Doc 20 ms am thanh vao ring buffer, tra ve RMS cua doan do
static float readChunk() {
  size_t br = 0;
  i2s_read(I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
  int n = br / 4;
  double sum = 0;
  for (int i = 0; i < n; i++) {
    float x = (float)(raw[i] >> VOICE_GAIN_SHIFT);
    float y = x - prevX + 0.995f * prevY;
    prevX = x; prevY = y;
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    int16_t s = (int16_t)y;
    ring[wpos & RING_MASK] = s;
    wpos++;
    sum += (double)s * s;
  }
  return n ? sqrtf(sum / n) : 0;
}

static void flush(int chunks) { for (int i = 0; i < chunks; i++) readChunk(); }

static void classifyFrom(uint32_t start, bool manual) {
  for (int i = 0; i < FE_N; i++) clip[i] = ring[(start + i) & RING_MASK];
  VoiceResult r;
  r.n = NUM_LABELS;
  r.manual = manual;
  uint32_t t0 = millis();
  vai::classify(clip, bufA, bufB, r.prob);
  r.ms = millis() - t0;
  xQueueSend(resultQ, &r, 0);
  flush(10);                                // bo du lieu tre trong luc tinh toan
}

static void runSelfTest() {
  float p[NUM_LABELS];
  uint32_t t0 = millis();
  vai::classify(TEST_AUDIO, bufA, bufB, p);
  uint32_t dt = millis() - t0;
  float maxd = 0;
  int am = 0, ae = 0;
  for (int i = 0; i < NUM_LABELS; i++) {
    float d = fabsf(p[i] - TEST_EXPECTED[i]);
    if (d > maxd) maxd = d;
    if (p[i] > p[am]) am = i;
    if (TEST_EXPECTED[i] > TEST_EXPECTED[ae]) ae = i;
  }
  selfTestOK = (am == ae) && maxd < 0.1f;
  Serial.printf("[GIONG] Tu kiem tra mo hinh: %s  (sai lech %.3f, %u ms)\n",
                selfTestOK ? "DAT" : "LOI", maxd, (unsigned)dt);
}

static void voiceTask(void *) {
  runSelfTest();
  flush(20);                                // mic on dinh
  float noiseFloor = 200;
  int loud = 0;
  const float preroll = VOICE_PREROLL_MS * FE_SR / 1000;

  for (;;) {
    float rms = readChunk();
    uint32_t now = millis();

    if (manualReq) {                        // phim 'A': ghi 2 giay ngay
      manualReq = false;
      uint32_t start = wpos;
      while (wpos - start < (uint32_t)FE_N) readChunk();
      classifyFrom(start, true);
      loud = 0;
      continue;
    }
    if (!VOICE_AUTO || (int32_t)(now - muteUntil) < 0) { loud = 0; continue; }

    float thr = max((float)VAD_MIN_RMS, noiseFloor * VAD_RATIO);
    if (rms > thr) {
      loud++;
    } else {
      loud = 0;
      noiseFloor = 0.98f * noiseFloor + 0.02f * rms;  // cap nhat tieng nen khi yen lang
    }

    if (loud >= 3) {                        // 60 ms to lien tuc -> co the la tieng noi
      uint32_t start = wpos - 3 * CHUNK - (uint32_t)preroll;
      int speechMs = 60;
      while (wpos - start < (uint32_t)FE_N) {
        if (readChunk() > thr) speechMs += 20;
      }
      if (speechMs >= VAD_MIN_SPEECH_MS) classifyFrom(start, false);
      loud = 0;
      voiceMute(VOICE_COOLDOWN_MS);
    }
  }
}

// ------------------------------------------------------------------ API
bool voiceBegin() {
  ring = (int16_t *)ps_malloc(RING_LEN * sizeof(int16_t));
  clip = (int16_t *)ps_malloc(FE_N * sizeof(int16_t));
  bufA = (float *)ps_malloc(FE_NFRAMES * FE_NMEL * 8 * sizeof(float));
  bufB = (float *)ps_malloc(FE_NFRAMES * FE_NMEL * 8 * sizeof(float));
  if (!ring || !clip || !bufA || !bufB) {
    Serial.println("[GIONG] LOI: khong cap phat duoc PSRAM - kiem tra Tools > PSRAM: OPI PSRAM");
    return false;
  }
  memset(ring, 0, RING_LEN * sizeof(int16_t));
  for (int i = 0; i < NUM_LABELS; i++)
    if (strcmp(LABEL_NAMES[i], "nen") == 0) noiseIdx = i;

  resultQ = xQueueCreate(4, sizeof(VoiceResult));
  i2sInit();
  xTaskCreatePinnedToCore(voiceTask, "voice", 8192, NULL, 2, NULL, 0);
  return true;
}

void voiceMute(uint32_t ms) {
  uint32_t t = millis() + ms;
  if ((int32_t)(t - muteUntil) > 0) muteUntil = t;
}
void voiceRequestManual() { manualReq = true; }
bool voiceGetResult(VoiceResult &r) { return resultQ && xQueueReceive(resultQ, &r, 0) == pdTRUE; }
int voiceNumLabels() { return NUM_LABELS; }
const char *voiceLabel(int i) { return (i >= 0 && i < NUM_LABELS) ? LABEL_NAMES[i] : "?"; }
int voiceOwnerIndex() { return IDX_CHU_NHA; }
int voiceNoiseIndex() { return noiseIdx; }
bool voiceSelfTestOK() { return selfTestOK; }
