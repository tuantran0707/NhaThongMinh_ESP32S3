/*
 * voice.cpp - Doc mic INMP441, phat hien tieng noi, chay mo hinh nhan dien giong chu nha
 * Xu ly am thanh GIONG HET code thu am: dich bit GAIN_SHIFT + loc chan DC.
 */
#include <Arduino.h>
#include <driver/i2s_std.h>
#include "pins.h"
#include "config.h"
#include "voice.h"
#include "voice_ai.h"
#include "test_vector.h"

#define I2S_PORT   I2S_NUM_0
#define CHUNK      320                    // toi da 20 ms moi lan doc
#define RING_LEN   65536                  // ~4 giay, luy thua cua 2
#define RING_MASK  (RING_LEN - 1)

static_assert(NUM_LABELS <= VOICE_MAX_LABELS, "Tang VOICE_MAX_LABELS de tranh tran VoiceResult::prob");
static_assert(FE_N <= RING_LEN, "RING_LEN phai lon hon hoac bang FE_N");

static QueueHandle_t resultQ = nullptr;
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t muteUntil = 0;
static volatile bool manualReq = false;
static bool selfTestOK = false;
static int noiseIdx = -1;

static int16_t *ring = nullptr;
static int16_t *clip = nullptr;
static float *bufA = nullptr, *bufB = nullptr;
static uint32_t wpos = 0;
static int32_t raw[CHUNK];
static float prevX = 0, prevY = 0;
static i2s_chan_handle_t rxChannel = nullptr;

static inline bool deadlinePending(uint32_t now, uint32_t deadline) {
  return deadline != 0 && (int32_t)(deadline - now) > 0;
}

static bool i2sInit() {
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
  channelConfig.dma_desc_num = 8;
  channelConfig.dma_frame_num = 256;
  esp_err_t err = i2s_new_channel(&channelConfig, nullptr, &rxChannel);
  if (err != ESP_OK) {
    Serial.printf("[GIONG] i2s_new_channel loi: %s\n", esp_err_to_name(err));
    return false;
  }

  i2s_std_config_t config = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FE_SR),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK_PIN,
      .ws = (gpio_num_t)I2S_WS_PIN,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)I2S_SD_PIN,
      .invert_flags = {},
    },
  };
  config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  err = i2s_channel_init_std_mode(rxChannel, &config);
  if (err != ESP_OK) {
    Serial.printf("[GIONG] i2s_channel_init_std_mode loi: %s\n", esp_err_to_name(err));
    i2s_del_channel(rxChannel);
    rxChannel = nullptr;
    return false;
  }
  err = i2s_channel_enable(rxChannel);
  if (err != ESP_OK) {
    Serial.printf("[GIONG] i2s_channel_enable loi: %s\n", esp_err_to_name(err));
    i2s_del_channel(rxChannel);
    rxChannel = nullptr;
    return false;
  }
  return true;
}

static void i2sEnd() {
  if (!rxChannel) return;
  i2s_channel_disable(rxChannel);
  i2s_del_channel(rxChannel);
  rxChannel = nullptr;
}

// Doc toi da 20 ms am thanh vao ring. Tra ve so mau doc duoc, 0 neu timeout/loi.
static int readChunk(float &rms) {
  size_t br = 0;
  esp_err_t err = i2s_channel_read(rxChannel, raw, sizeof(raw), &br, 100);
  if (err != ESP_OK || br == 0) {
    static uint32_t lastLog = 0;
    uint32_t now = millis();
    if (err != ESP_ERR_TIMEOUT && now - lastLog >= 2000) {
      lastLog = now;
      Serial.printf("[GIONG] i2s_read loi: %s, bytes=%u\n", esp_err_to_name(err), (unsigned)br);
    }
    rms = 0;
    return 0;
  }

  int n = br / sizeof(raw[0]);
  double sum = 0;
  for (int i = 0; i < n; i++) {
    float x = (float)(raw[i] >> VOICE_GAIN_SHIFT);
    float y = x - prevX + 0.995f * prevY;
    prevX = x;
    prevY = y;
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    int16_t s = (int16_t)y;
    ring[wpos & RING_MASK] = s;
    wpos++;
    sum += (double)s * s;
  }
  rms = n ? sqrtf(sum / n) : 0;
  return n;
}

static void flush(int chunks) {
  float rms;
  for (int i = 0; i < chunks; i++) readChunk(rms);
}

static bool captureSamples(uint32_t count, uint32_t timeoutMs) {
  uint32_t startPos = wpos;
  uint32_t started = millis();
  float rms;
  while (wpos - startPos < count) {
    readChunk(rms);
    if (millis() - started >= timeoutMs) {
      Serial.println("[GIONG] Timeout khi thu am");
      return false;
    }
  }
  return true;
}

static void classifyFrom(uint32_t start, bool manual) {
  for (int i = 0; i < FE_N; i++) clip[i] = ring[(start + i) & RING_MASK];
  VoiceResult r = {};
  r.n = NUM_LABELS;
  r.manual = manual;
  uint32_t t0 = millis();
  vai::classify(clip, bufA, bufB, r.prob);
  r.ms = millis() - t0;
  if (xQueueSend(resultQ, &r, 0) != pdTRUE)
    Serial.println("[GIONG] Hang doi ket qua day, bo ket qua moi");
  flush(10);
}

static bool runSelfTest() {
  float p[NUM_LABELS];
  uint32_t t0 = millis();
  vai::classify(TEST_AUDIO, bufA, bufB, p);
  uint32_t dt = millis() - t0;
  float maxd = 0;
  int am = 0, ae = 0;
  bool finite = true;
  for (int i = 0; i < NUM_LABELS; i++) {
    finite = finite && isfinite(p[i]);
    float d = fabsf(p[i] - TEST_EXPECTED[i]);
    if (d > maxd) maxd = d;
    if (p[i] > p[am]) am = i;
    if (TEST_EXPECTED[i] > TEST_EXPECTED[ae]) ae = i;
  }
  selfTestOK = finite && (am == ae) && maxd < 0.1f;
  Serial.printf("[GIONG] Tu kiem tra mo hinh: %s (sai lech %.3f, %u ms)\n",
                selfTestOK ? "DAT" : "LOI", maxd, (unsigned)dt);
  return selfTestOK;
}

static bool takeManualRequest(uint32_t now) {
  portENTER_CRITICAL(&stateMux);
  bool requested = manualReq && !deadlinePending(now, muteUntil);
  if (requested) manualReq = false;
  portEXIT_CRITICAL(&stateMux);
  return requested;
}

static bool isMuted(uint32_t now) {
  portENTER_CRITICAL(&stateMux);
  if (muteUntil && !deadlinePending(now, muteUntil)) muteUntil = 0;
  bool muted = deadlinePending(now, muteUntil);
  portEXIT_CRITICAL(&stateMux);
  return muted;
}

static void voiceTask(void *) {
  flush(20);
  float noiseFloor = 200;
  uint32_t loudSamples = 0;
  const uint32_t prerollSamples = VOICE_PREROLL_MS * FE_SR / 1000;
  const uint32_t triggerSamples = 60 * FE_SR / 1000;

  for (;;) {
    float rms = 0;
    int n = readChunk(rms);
    if (n == 0) continue;
    uint32_t now = millis();

    if (takeManualRequest(now)) {
      uint32_t start = wpos;
      if (captureSamples(FE_N, 4000)) classifyFrom(start, true);
      loudSamples = 0;
      continue;
    }
    if (!VOICE_AUTO || isMuted(now)) {
      loudSamples = 0;
      continue;
    }

    float thr = max((float)VAD_MIN_RMS, noiseFloor * VAD_RATIO);
    if (rms > thr) {
      loudSamples += n;
    } else {
      loudSamples = 0;
      noiseFloor = 0.98f * noiseFloor + 0.02f * rms;
    }

    if (loudSamples >= triggerSamples) {
      uint32_t lookback = loudSamples + prerollSamples;
      if (lookback > wpos) lookback = wpos;
      uint32_t start = wpos - lookback;
      uint32_t speechSamples = loudSamples;
      uint32_t captureStarted = millis();
      bool captureOK = true;

      while (wpos - start < (uint32_t)FE_N) {
        float partRms = 0;
        int partN = readChunk(partRms);
        if (partN > 0 && partRms > thr) speechSamples += partN;
        if (millis() - captureStarted >= 4000) {
          captureOK = false;
          Serial.println("[GIONG] Timeout khi VAD thu am");
          break;
        }
      }
      if (captureOK && speechSamples * 1000UL / FE_SR >= VAD_MIN_SPEECH_MS)
        classifyFrom(start, false);
      loudSamples = 0;
      voiceMute(VOICE_COOLDOWN_MS);
    }
  }
}

static void freeBuffers() {
  free(ring);  ring = nullptr;
  free(clip);  clip = nullptr;
  free(bufA);  bufA = nullptr;
  free(bufB);  bufB = nullptr;
}

bool voiceBegin() {
  ring = (int16_t *)ps_malloc(RING_LEN * sizeof(int16_t));
  clip = (int16_t *)ps_malloc(FE_N * sizeof(int16_t));
  bufA = (float *)ps_malloc(FE_NFRAMES * FE_NMEL * 8 * sizeof(float));
  bufB = (float *)ps_malloc(FE_NFRAMES * FE_NMEL * 8 * sizeof(float));
  if (!ring || !clip || !bufA || !bufB) {
    Serial.println("[GIONG] LOI: khong cap phat duoc PSRAM - chon OPI PSRAM");
    freeBuffers();
    return false;
  }
  memset(ring, 0, RING_LEN * sizeof(int16_t));
  for (int i = 0; i < NUM_LABELS; i++)
    if (strcmp(LABEL_NAMES[i], "nen") == 0) noiseIdx = i;

  resultQ = xQueueCreate(4, sizeof(VoiceResult));
  if (!resultQ) {
    Serial.println("[GIONG] LOI: khong tao duoc hang doi ket qua");
    freeBuffers();
    return false;
  }
  if (!i2sInit()) {
    vQueueDelete(resultQ);
    resultQ = nullptr;
    freeBuffers();
    return false;
  }
  if (!runSelfTest()) {
    Serial.println("[GIONG] Da khoa nhan dien vi self-test that bai");
    i2sEnd();
    vQueueDelete(resultQ);
    resultQ = nullptr;
    freeBuffers();
    return false;
  }

  BaseType_t created = xTaskCreatePinnedToCore(voiceTask, "voice", 8192, nullptr, 2, nullptr, 0);
  if (created != pdPASS) {
    Serial.println("[GIONG] LOI: khong tao duoc task voice");
    i2sEnd();
    vQueueDelete(resultQ);
    resultQ = nullptr;
    freeBuffers();
    selfTestOK = false;
    return false;
  }
  return true;
}

void voiceMute(uint32_t ms) {
  uint32_t now = millis();
  uint32_t requestedUntil = now + ms;
  portENTER_CRITICAL(&stateMux);
  bool currentPending = deadlinePending(now, muteUntil);
  if (!currentPending || (int32_t)(requestedUntil - muteUntil) > 0) muteUntil = requestedUntil;
  portEXIT_CRITICAL(&stateMux);
}

void voiceRequestManual() {
  portENTER_CRITICAL(&stateMux);
  manualReq = true;
  portEXIT_CRITICAL(&stateMux);
}

bool voiceGetResult(VoiceResult &r) {
  return resultQ && xQueueReceive(resultQ, &r, 0) == pdTRUE;
}
int voiceNumLabels() { return NUM_LABELS; }
const char *voiceLabel(int i) { return (i >= 0 && i < NUM_LABELS) ? LABEL_NAMES[i] : "?"; }
int voiceOwnerIndex() { return IDX_CHU_NHA; }
int voiceNoiseIndex() { return noiseIdx; }
bool voiceSelfTestOK() { return selfTestOK; }
