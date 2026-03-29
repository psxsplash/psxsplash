#pragma once

#include <stdint.h>

namespace psxsplash {

static constexpr int MAX_AUDIO_CLIPS = 32;

static constexpr int MAX_VOICES = 24;

static constexpr uint32_t SPU_RAM_START = 0x1010;
static constexpr uint32_t SPU_RAM_END = 0x80000;

static constexpr uint32_t DEFAULT_ADSR = 0x000A000F;

struct AudioClip {
  uint32_t spuAddr;
  uint32_t size;
  uint16_t sampleRate;
  bool loop;
  bool loaded;
};

/// Manages SPU voices and audio clip playback.
///
///   init()
///   loadClip(index, data, size, rate, loop) -> bool
///   play(clipIndex)                         -> channel
///   play(clipIndex, volume, pan)            -> channel
///   stopVoice(channel)
///   stopAll()
///   setVoiceVolume(channel, vol, pan)
///
/// Volume is 0-128 (0=silent, 128=max). Pan is 0-127 (64=center).
class AudioManager {
public:
  /// Initialize SPU hardware and reset state
  void init();

  /// Upload ADPCM data to SPU RAM and register as clip index.
  /// Data must be 16-byte aligned. Returns true on success.
  bool loadClip(int clipIndex, const uint8_t *adpcmData, uint32_t sizeBytes,
                uint16_t sampleRate, bool loop);

  /// Play a clip by index. Returns channel (0-23), or -1 if full.
  /// Volume: 0-128 (128=max). Pan: 0 (left) to 127 (right), 64 = center.
  int play(int clipIndex, int volume = 128, int pan = 64);

  /// Stop a specific channel
  void stopVoice(int channel);

  /// Stop all playing channels
  void stopAll();

  /// Set volume/pan on a playing channel
  void setVoiceVolume(int channel, int volume, int pan = 64);

  /// Get total SPU RAM used by loaded clips
  uint32_t getUsedSPURam() const { return m_nextAddr - SPU_RAM_START; }

  /// Get total SPU RAM available
  uint32_t getTotalSPURam() const { return SPU_RAM_END - SPU_RAM_START; }

  /// Get number of loaded clips
  int getLoadedClipCount() const;

  /// Reset all clips and free SPU RAM
  void reset();

private:
  /// Convert 0-128 volume to hardware 0-0x3FFF (fixed-volume mode)
  static uint16_t volToHw(int v);

  AudioClip m_clips[MAX_AUDIO_CLIPS];
  uint32_t m_nextAddr = SPU_RAM_START;
};

} // namespace psxsplash
