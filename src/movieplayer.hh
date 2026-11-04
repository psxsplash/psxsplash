#pragma once

#include "features.hh"

#if PSXSPLASH_FEATURE_MOVIE

#include <stdint.h>

#include <psyqo/gpu.hh>

#include "controls.hh"
#include "srt.hh"

namespace psxsplash {

/**
 * MoviePlayer - full-screen STR movies off the disc, through psyqo's StrPlayer.
 *
 * A movie is MOVIES/<NAME>.STR;1 on the disc, and it plays one of two ways:
 *
 *  - In place: the scene stays in RAM and simply stops ticking, and the game
 *    carries on from where it was when the movie ends. The picture is 15-bit,
 *    which fits the two 320x240 framebuffers, so no texture in VRAM is touched
 *    and nothing has to be saved or reloaded. It needs the player's buffers to
 *    fit in the heap beside the scene, about 200 KB for 320x240.
 *
 *  - Then a scene: the scene is freed first and scene N loads when the movie
 *    ends. The picture is 24-bit by default, which spills past the framebuffers
 *    into VRAM the next scene reloads anyway, and it fits with a scene of any
 *    size. This is the one for logos and chapter cutscenes.
 *
 * Disc builds only: the player streams sectors off the drive, which a PCdrv
 * build does not have.
 *
 * Nothing else runs while a movie plays: no game logic, no scene audio, and no
 * Lua but the movie's own onUpdate. Scene voices and CD music are stopped first;
 * a script restarts its music in onComplete. onUpdate runs every frame with the
 * frames shown and the pad, and Movie.Stop() in it ends the movie there: a
 * decision counted in the movie's frames, for a game made of movies. It runs
 * while the movie streams, so what reads the disc waits for onComplete. Without
 * it, a movie that loops can only end by the pad, so it is always skippable.
 *
 * In place, a movie smaller than the screen plays on black, or with `overlay`
 * as a window over the frozen last frame, at `x`, `y` or else centered.
 */
class MoviePlayer {
  public:
    enum class Result : uint8_t { ENDED, SKIPPED, FAILED, STOPPED };

    static constexpr int c_noRef = -2;  // LUA_NOREF

    // One line of text over a 15-bit movie, split on \n into at most two rows. Times
    // are from the first picture. They come from MOVIES/NAME.SRT beside the movie, when
    // there is one, and make it 15-bit: the GPU cannot draw into a 24-bit picture.
    using Subtitle = srt::Subtitle;

    struct Request {
        char name[9] = {};      // "INTRO": A-Z, 0-9 and _, 8 characters at most
        int thenScene = -1;     // the scene to load after it, or -1 to stay
        int onComplete = c_noRef;
        int onUpdate = c_noRef;  // every frame it plays, with the frames shown
        int16_t x = -1, y = -1;  // the picture's corner, -1 to center it
        bool skippable = false;
        bool rgb24 = false;
        bool loop = false;
        bool overlay = false;
        uint8_t speed = 0;  // 1 for a movie made with psxavenc -x 1, 2 for -x 2, 0 for the disc's
        Subtitle* subtitles = nullptr;  // from psyqo_malloc; the player frees them
        uint16_t subtitleCount = 0;
        // A 16-bit UI image's rectangle, for a movie that plays behind the scene.
        int16_t vramX = -1, vramY = 0;
        uint16_t vramWidth = 0, vramHeight = 0;
        bool behind() const { return vramX >= 0; }
    };

    // Queues a movie for the end of this frame. False while one is queued or
    // playing, and on a build that cannot play movies.
    bool request(const Request& request);
    bool pending() const { return m_pending; }
    bool busy() const { return m_pending || m_playing; }
    bool playing() const { return m_playing; }

    // Takes the queued request. Scene teardown happens between this and start().
    const Request& takeRequest() {
        m_pending = false;
        return m_request;
    }

    // The drive must be idle. False if the file is missing or the player's
    // buffers do not fit; nothing is left running then.
    bool start(psyqo::GPU& gpu, Controls& pad);

    // One frame, from the scene's frame(). True once it is over, with `result` set.
    bool frame(Controls& pad, Result& result);

    const Request& request() const { return m_request; }

    // Movie.Stop: the movie playing ends at its next frame(), its picture up.
    bool askStop() {
        if (!m_playing) return false;
        m_stopAsked = true;
        return true;
    }
    bool stopAsked() const { return m_stopAsked; }

    // Frames shown so far, for a scene that follows the movie.
    uint32_t framesShown() const;
    uint32_t lastShown() const { return m_lastShown; }
    uint32_t lastDropped() const { return m_lastDropped; }

    // The scene calls this when a movie has ended, or never started.
    void releaseSubtitles();

    // The scene behind the movie is going: it stops where it is, and nothing of it runs.
    void abandon();
    static void loadSubtitles(class FileLoaderCDRom& loader, Request& request);

    // The display back as the game left it, once no movie follows: a movie that
    // ends leaves its last picture up, so one that follows starts from it.
    void restoreDisplay();

  private:
    bool skipPressed(Controls& pad);
    void drawSubtitles();
    void finish();

    Request m_request;
    psyqo::GPU* m_gpu = nullptr;
    bool m_pending = false;
    bool m_playing = false;
    bool m_skipHeld = false;
    bool m_stopAsked = false;
    bool m_clockStarted = false;
    uint32_t m_lastShown = 0, m_lastDropped = 0;
    uint32_t m_clockStart = 0;
    int16_t m_picX = 0, m_picY = 0, m_picW = 0, m_picH = 0;  // the picture in the display, for the subtitles
    bool m_held = false;       // a movie's last picture is up
    bool m_heldRgb24 = false;  // at 24 bits
};

}  // namespace psxsplash

#endif  // PSXSPLASH_FEATURE_MOVIE
