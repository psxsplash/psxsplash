#include "audiomanager.hh"

#include "common/hardware/dma.h"
#include "common/hardware/spu.h"
#include <psyqo/kernel.hh>
#include <psyqo/spu.hh>
#include <psyqo/xprintf.h>

namespace psxsplash {

uint16_t AudioManager::volToHw(int v) {
  if (v <= 0)
    return 0;
  if (v >= 128)
    return 0x3fff;
  return static_cast<uint16_t>((v * 0x3fff) / 128);
}

void AudioManager::init() {
  psyqo::SPU::initialize();

  m_nextAddr = SPU_RAM_START;

  for (int i = 0; i < MAX_AUDIO_CLIPS; i++) {
    m_clips[i].loaded = false;
  }
}

void AudioManager::reset() {
  stopAll();
  for (int i = 0; i < MAX_AUDIO_CLIPS; i++) {
    m_clips[i].loaded = false;
  }
  m_nextAddr = SPU_RAM_START;
}

bool AudioManager::loadClip(int clipIndex, const uint8_t *adpcmData,
                            uint32_t sizeBytes, uint16_t sampleRate,
                            bool loop) {
  if (clipIndex < 0 || clipIndex >= MAX_AUDIO_CLIPS)
    return false;
  if (!adpcmData || sizeBytes == 0)
    return false;

  // check for and skip VAG header if present
  if (sizeBytes >= 48) {
    const char *magic = reinterpret_cast<const char *>(adpcmData);
    if (magic[0] == 'V' && magic[1] == 'A' && magic[2] == 'G' &&
        magic[3] == 'p') {
      adpcmData += 48;
      sizeBytes -= 48;
    }
  }

  uint32_t addr = (m_nextAddr + 15) & ~15u;
  uint32_t alignedSize = (sizeBytes + 15) & ~15u;

  if (addr + alignedSize > SPU_RAM_END) {
    return false;
  }

  const uint8_t *src = adpcmData;
  uint32_t remaining = alignedSize;
  uint32_t dstAddr = addr;
  while (remaining > 0) {
    uint32_t bytesThisRound = (remaining > 65520u) ? 65520u : remaining;
    bytesThisRound &= ~15u; // 16-byte block alignment
    if (bytesThisRound == 0)
      break;

    uint16_t dmaSizeParam = (uint16_t)(bytesThisRound / 4);
    psyqo::SPU::dmaWrite(dstAddr, src, dmaSizeParam, 4);

    while (DMA_CTRL[DMA_SPU].CHCR & (1 << 24)) {
    }

    src += bytesThisRound;
    dstAddr += bytesThisRound;
    remaining -= bytesThisRound;
  }

  SPU_CTRL &= ~(0b11 << 4);

  m_clips[clipIndex].spuAddr = addr;
  m_clips[clipIndex].size = sizeBytes;
  m_clips[clipIndex].sampleRate = sampleRate;
  m_clips[clipIndex].loop = loop;
  m_clips[clipIndex].loaded = true;

  m_nextAddr = addr + alignedSize;
  return true;
}

int AudioManager::play(int clipIndex, int volume, int pan) {
  if (clipIndex < 0 || clipIndex >= MAX_AUDIO_CLIPS ||
      !m_clips[clipIndex].loaded) {
    return -1;
  }

  uint32_t ch = psyqo::SPU::getNextFreeChannel();
  if (ch == psyqo::SPU::NO_FREE_CHANNEL)
    return -1;

  const AudioClip &clip = m_clips[clipIndex];

  uint16_t vol = volToHw(volume);
  uint16_t leftVol = vol;
  uint16_t rightVol = vol;
  if (pan != 64) {
    int p = pan < 0 ? 0 : (pan > 127 ? 127 : pan);
    leftVol = (uint16_t)((uint32_t)vol * (127 - p) / 127);
    rightVol = (uint16_t)((uint32_t)vol * p / 127);
  }

  constexpr uint16_t DUMMY_SPU_ADDR = 0x1000;
  if (clip.loop) {
    SPU_VOICES[ch].sampleRepeatAddr = static_cast<uint16_t>(clip.spuAddr / 8);
  } else {
    SPU_VOICES[ch].sampleRepeatAddr = DUMMY_SPU_ADDR / 8;
  }

  psyqo::SPU::ChannelPlaybackConfig config;
  config.sampleRate.value =
      static_cast<uint16_t>(((uint32_t)clip.sampleRate << 12) / 44100);
  config.volumeLeft = leftVol;
  config.volumeRight = rightVol;
  config.adsr = DEFAULT_ADSR;

  if (ch > 15) {
    SPU_KEY_OFF_HIGH = 1 << (ch - 16);
  } else {
    SPU_KEY_OFF_LOW = 1 << ch;
  }

  SPU_VOICES[ch].volumeLeft = config.volumeLeft;
  SPU_VOICES[ch].volumeRight = config.volumeRight;
  SPU_VOICES[ch].sampleRate = config.sampleRate.value;
  SPU_VOICES[ch].sampleStartAddr = static_cast<uint16_t>(clip.spuAddr / 8);
  SPU_VOICES[ch].ad = config.adsr & 0xFFFF;
  SPU_VOICES[ch].sr = (config.adsr >> 16) & 0xFFFF;

  if (ch > 15) {
    SPU_KEY_ON_HIGH = 1 << (ch - 16);
  } else {
    SPU_KEY_ON_LOW = 1 << ch;
  }

  return static_cast<int>(ch);
}

void AudioManager::stopVoice(int channel) {
  if (channel < 0 || channel >= MAX_VOICES)
    return;
  psyqo::SPU::silenceChannels(1u << channel);
}

void AudioManager::stopAll() { psyqo::SPU::silenceChannels(0x00FFFFFFu); }

void AudioManager::setVoiceVolume(int channel, int volume, int pan) {
  if (channel < 0 || channel >= MAX_VOICES)
    return;
  uint16_t vol = volToHw(volume);
  if (pan == 64) {
    SPU_VOICES[channel].volumeLeft = vol;
    SPU_VOICES[channel].volumeRight = vol;
  } else {
    int p = pan < 0 ? 0 : (pan > 127 ? 127 : pan);
    SPU_VOICES[channel].volumeLeft =
        (uint16_t)((uint32_t)vol * (127 - p) / 127);
    SPU_VOICES[channel].volumeRight = (uint16_t)((uint32_t)vol * p / 127);
  }
}

int AudioManager::getLoadedClipCount() const {
  int count = 0;
  for (int i = 0; i < MAX_AUDIO_CLIPS; i++) {
    if (m_clips[i].loaded)
      count++;
  }
  return count;
}

} // namespace psxsplash
