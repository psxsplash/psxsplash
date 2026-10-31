#pragma once

#include "features.hh"

#if PSXSPLASH_FEATURE_MOVIE

#include <stdint.h>

#include <psyqo/gpu.hh>

#include "gameobject.hh"

namespace psxsplash {

/**
 * MovieTexture - a small movie on a mesh, with the game running around it.
 *
 * The movie is MOVIES/<NAME>.SBS (psxavenc -t sbs -a <bytes>): a row of MDEC
 * frames of one size, each complete in itself. The file is read into the heap
 * when it starts, and a frame at a time decodes straight into the target's
 * texture where it sits in VRAM, so the mesh needs no new material and the
 * renderer knows nothing about it: a TV in a room is a quad mapped to a 16-bit
 * texture, and every mesh sharing that texture shows the movie too. The picture
 * has the size of the texture's rectangle, which the target's UVs give, so both
 * sides have to be multiples of 16. The texture keeps the last frame shown until
 * the scene reloads.
 *
 * One at a time. Disc builds only, and it needs the drive for a moment to read
 * the file: start it before CD music does, which is why this reads the file as it
 * is asked rather than at the end of the frame.
 */
class MovieTexture {
  public:
    static constexpr int c_noRef = -2;  // LUA_NOREF

    struct Options {
        uint8_t fps = 15;
        bool loop = true;
        int onComplete = c_noRef;
    };

    // False, with a log line, when the target has no 16-bit texture, the file is
    // not on the disc, or it does not fit in the heap. Nothing is left running then.
    bool play(const char* name, const GameObject* target, const Options& options);

    // Lets go of the file and the buffers; the texture keeps its last frame.
    void stop();

    bool playing() const { return m_file != nullptr; }

    // Once a frame, with the frame's work done. True when a movie that does not
    // loop has shown its last frame.
    bool tick(psyqo::GPU& gpu);

    // The script's onComplete, which the caller now owns.
    int takeOnComplete() {
        const int ref = m_onComplete;
        m_onComplete = c_noRef;
        return ref;
    }

  private:
    bool decode(uint32_t index, psyqo::GPU& gpu);

    uint8_t* m_file = nullptr;
    uint16_t* m_runLevels = nullptr;
    uint16_t* m_pixels = nullptr;
    uint32_t m_runLevelHalfwords = 0;
    uint32_t m_stride = 0;
    uint32_t m_frames = 0;
    uint32_t m_macroblocks = 0;
    uint32_t m_startVsync = 0;
    uint32_t m_shown = 0;
    int16_t m_x = 0, m_y = 0, m_width = 0, m_height = 0;
    int m_onComplete = c_noRef;
    uint32_t m_stalls = 0;
    uint8_t m_fps = 15;
    bool m_loop = true;
    char m_name[9] = {};
};

}  // namespace psxsplash

#endif  // PSXSPLASH_FEATURE_MOVIE
