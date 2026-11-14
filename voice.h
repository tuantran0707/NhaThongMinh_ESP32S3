/*
 * voice.h - Nhan dien giong noi chay rieng tren Core 0
 */
#pragma once
#include <Arduino.h>

#define VOICE_MAX_LABELS 8

struct VoiceResult {
  float prob[VOICE_MAX_LABELS];
  uint8_t n;          // so nhan
  bool manual;        // true = do bam phim 'A'
  uint32_t ms;        // thoi gian xu ly
};

bool        voiceBegin();                  // khoi dong mic + task nhan dien
void        voiceMute(uint32_t ms);        // bo qua mic trong ms (khi loa/coi/servo dang chay)
void        voiceRequestManual();          // ghi ngay 2 giay roi nhan dien (phim 'A')
bool        voiceGetResult(VoiceResult &r);
int         voiceNumLabels();
const char *voiceLabel(int i);
int         voiceOwnerIndex();
int         voiceNoiseIndex();             // -1 neu mo hinh khong co nhan "nen"
bool        voiceSelfTestOK();
