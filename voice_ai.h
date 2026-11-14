/*
 * voice_ai.h - Trich dac trung log-mel + mang CNN nhan dien giong chu nha
 * Viet bang C++ thuan (khong can thu vien TensorFlow), tinh GIONG HET luc train tren Colab.
 * Chi duoc #include trong 1 file .cpp (voice.cpp).
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "feature_config.h"
#include "nn_weights.h"

namespace vai {

// ------------------------------------------------------------------ FFT 512 diem
static float tw_re[FE_NFFT / 2], tw_im[FE_NFFT / 2], hann[FE_NFFT];
static uint16_t bitrev[FE_NFFT];
static bool inited = false;

static void init() {
  if (inited) return;
  int bits = 0;
  while ((1 << bits) < FE_NFFT) bits++;
  for (int i = 0; i < FE_NFFT; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    bitrev[i] = r;
    hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / FE_NFFT);
  }
  for (int k = 0; k < FE_NFFT / 2; k++) {
    tw_re[k] = cosf(-2.0f * (float)M_PI * k / FE_NFFT);
    tw_im[k] = sinf(-2.0f * (float)M_PI * k / FE_NFFT);
  }
  inited = true;
}

static void fft(float *re, float *im) {
  for (int i = 0; i < FE_NFFT; i++) {
    int j = bitrev[i];
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= FE_NFFT; len <<= 1) {
    int half = len >> 1, step = FE_NFFT / len;
    for (int s = 0; s < FE_NFFT; s += len) {
      for (int k = 0; k < half; k++) {
        float wr = tw_re[k * step], wi = tw_im[k * step];
        int a = s + k, b = a + half;
        float xr = re[b] * wr - im[b] * wi;
        float xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr;        im[a] += xi;
      }
    }
  }
}

// ------------------------------------------------------------------ Dac trung
// audio: FE_N mau int16  ->  feat: FE_NFRAMES*FE_NMEL (da chuan hoa (x-MEAN)/STD)
static void features(const int16_t *audio, float *feat) {
  init();
  static float re[FE_NFFT], im[FE_NFFT], pw[FE_NBINS];
  for (int f = 0; f < FE_NFRAMES; f++) {
    const int16_t *x = audio + f * FE_HOP;
    for (int i = 0; i < FE_NFFT; i++) { re[i] = (x[i] / 32768.0f) * hann[i]; im[i] = 0; }
    fft(re, im);
    for (int k = 0; k < FE_NBINS; k++) pw[k] = re[k] * re[k] + im[k] * im[k];
    for (int m = 0; m < FE_NMEL; m++) {
      const float *fb = MEL_FB + m * FE_NBINS;
      float s = 0;
      for (int k = 0; k < FE_NBINS; k++) if (fb[k] != 0.0f) s += pw[k] * fb[k];
      feat[f * FE_NMEL + m] = (logf(s + FE_LOG_EPS) - FE_MEAN) / FE_STD;
    }
  }
}

// ------------------------------------------------------------------ Cac lop mang
// Conv 3x3, stride 1, padding SAME, + ReLU.  in[H][W][C], w[OC][3][3][C]
static void conv3x3_relu(const float *in, int H, int W, int C,
                         const float *w, const float *b, int OC, float *out) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      for (int o = 0; o < OC; o++) {
        float s = b[o];
        const float *wo = w + o * 9 * C;
        for (int ky = 0; ky < 3; ky++) {
          int iy = y + ky - 1;
          if (iy < 0 || iy >= H) continue;
          for (int kx = 0; kx < 3; kx++) {
            int ix = x + kx - 1;
            if (ix < 0 || ix >= W) continue;
            const float *pi = in + (iy * W + ix) * C;
            const float *pw = wo + (ky * 3 + kx) * C;
            for (int c = 0; c < C; c++) s += pi[c] * pw[c];
          }
        }
        out[(y * W + x) * OC + o] = s > 0 ? s : 0;
      }
}

// MaxPool 2x2 stride 2 (valid)
static void maxpool2(const float *in, int H, int W, int C, float *out) {
  int OH = H / 2, OW = W / 2;
  for (int y = 0; y < OH; y++)
    for (int x = 0; x < OW; x++)
      for (int c = 0; c < C; c++) {
        float m = in[((2 * y) * W + 2 * x) * C + c];
        float v;
        v = in[((2 * y) * W + 2 * x + 1) * C + c];     if (v > m) m = v;
        v = in[((2 * y + 1) * W + 2 * x) * C + c];     if (v > m) m = v;
        v = in[((2 * y + 1) * W + 2 * x + 1) * C + c]; if (v > m) m = v;
        out[(y * OW + x) * C + c] = m;
      }
}

static void dense(const float *in, int N, const float *w, const float *b, int M, float *out, bool relu) {
  for (int o = 0; o < M; o++) {
    float s = b[o];
    for (int i = 0; i < N; i++) s += in[i] * w[o * N + i];
    out[o] = (relu && s < 0) ? 0 : s;
  }
}

// ------------------------------------------------------------------ Chay mang
// bufA, bufB: moi buffer >= FE_NFRAMES*FE_NMEL*8 float (~127 KB, dat trong PSRAM)
// feat nam trong bufA khi goi. Ket qua: prob[NUM_LABELS]
static void run_net(float *bufA, float *bufB, float *prob) {
  const int H0 = FE_NFRAMES, W0 = FE_NMEL;                 // 99 x 40
  conv3x3_relu(bufA, H0, W0, 1, C1_W, C1_B, 8, bufB);       // 99x40x8
  maxpool2(bufB, H0, W0, 8, bufA);                          // 49x20x8
  int H1 = H0 / 2, W1 = W0 / 2;
  conv3x3_relu(bufA, H1, W1, 8, C2_W, C2_B, 16, bufB);      // 49x20x16
  maxpool2(bufB, H1, W1, 16, bufA);                         // 24x10x16
  int H2 = H1 / 2, W2 = W1 / 2;
  conv3x3_relu(bufA, H2, W2, 16, C3_W, C3_B, 32, bufB);     // 24x10x32
  maxpool2(bufB, H2, W2, 32, bufA);                         // 12x5x32
  int H3 = H2 / 2, W3 = W2 / 2;
  conv3x3_relu(bufA, H3, W3, 32, C4_W, C4_B, 48, bufB);     // 12x5x48

  float gap[48] = {0};                                      // Global average pooling
  for (int i = 0; i < H3 * W3; i++)
    for (int c = 0; c < 48; c++) gap[c] += bufB[i * 48 + c];
  for (int c = 0; c < 48; c++) gap[c] /= (H3 * W3);

  float h[32], logit[NUM_LABELS];
  dense(gap, 48, F1_W, F1_B, 32, h, true);
  dense(h, 32, F2_W, F2_B, NUM_LABELS, logit, false);

  float mx = logit[0];
  for (int i = 1; i < NUM_LABELS; i++) if (logit[i] > mx) mx = logit[i];
  float sum = 0;
  for (int i = 0; i < NUM_LABELS; i++) { prob[i] = expf(logit[i] - mx); sum += prob[i]; }
  for (int i = 0; i < NUM_LABELS; i++) prob[i] /= sum;
}

// Toan bo: audio 2 giay -> xac suat tung nhan
static void classify(const int16_t *audio, float *bufA, float *bufB, float *prob) {
  features(audio, bufA);
  run_net(bufA, bufB, prob);
}

}  // namespace vai
