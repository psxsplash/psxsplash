#pragma once

#include <stdint.h>

namespace psxsplash {

// The bookkeeping behind Audio.PlayCDDA / PauseCDDA / StopCDDA / ResumeCDDA,
// kept apart from the drive so the host tests can run it. MusicManager feeds it
// what Lua asks for and what psyqo's play callback reports, and sends the drive
// whatever it says.
//
// psyqo calls a successful play's callback twice, with true both times: once
// when the track starts, and once when it is over, whether it ran to its end or
// a pause or stop cut it short. So the second call cannot tell a finished track
// from a paused one; the phase here can: pause and stop move it to Stopping
// before the drive is told, so a callback that still finds it Playing means the
// track ran out. A looping track then comes round again from tick(), on the
// next frame, the same way a queued Audio.PlayCDDA starts.
class CDDAState {
  public:
    enum class Phase : uint8_t { Idle, Starting, Playing, Stopping };
    enum class Action : uint8_t { None, Play, Pause, Stop, Silence };

    // Audio.PlayCDDA(track, loop). Whatever played or looped before is done
    // with. True: play the track now. False: it waits for tick(), which first
    // pauses a track still playing, then returns Play once the drive is free.
    bool play(int track, bool loop, bool deviceIdle) {
        m_pendingTrack = -1;
        m_loop = false;
        m_stopAfterStart = false;
        if (m_phase == Phase::Idle && deviceIdle) {
            begin(track, loop);
            return true;
        }
        m_pendingTrack = track;
        m_pendingLoop = loop;
        return false;
    }

    // Audio.ResumeCDDA(). True: send the resume. A resumed track does not loop.
    bool resume(bool deviceIdle) {
        if (m_phase != Phase::Idle || m_track < 0 || m_pendingTrack >= 0 || !deviceIdle) return false;
        m_phase = Phase::Starting;
        m_silence = false;
        return true;
    }

    // Audio.PauseCDDA(). True: send the pause. Ends the loop either way.
    bool pause() {
        m_loop = false;
        m_pendingTrack = -1;
        if (m_phase != Phase::Playing) return false;
        m_phase = Phase::Stopping;
        return true;
    }

    // Audio.StopCDDA(). True: send the stop. Ends the loop and anything queued;
    // a track still starting is stopped by tick() once it plays.
    bool stop() {
        m_loop = false;
        m_pendingTrack = -1;
        m_track = -1;
        m_silence = true;
        if (m_phase == Phase::Starting) m_stopAfterStart = true;
        if (m_phase != Phase::Playing) return false;
        m_phase = Phase::Stopping;
        return true;
    }

    // A scene load or a movie takes the drive: nothing may come round again,
    // and nothing queued may start. (The caller stops a playing track itself.)
    void cancel() {
        m_loop = false;
        m_pendingTrack = -1;
    }

    // psyqo's play (or resume) callback.
    void callback(bool success) {
        switch (m_phase) {
            case Phase::Starting:
                if (success) {
                    m_phase = Phase::Playing;
                } else {  // no second call follows a failed start
                    m_phase = Phase::Idle;
                    m_track = -1;
                    m_loop = false;
                    m_stopAfterStart = false;
                }
                break;
            case Phase::Playing:  // nobody paused or stopped it: it ran to its end
                m_phase = Phase::Idle;
                if (m_loop && m_pendingTrack < 0) {
                    m_pendingTrack = m_track;
                    m_pendingLoop = true;
                } else {
                    m_track = -1;
                    m_silence = true;
                }
                break;
            case Phase::Stopping:  // the pause or stop we sent
                m_phase = Phase::Idle;
                break;
            case Phase::Idle:
                break;
        }
    }

    // Once per frame. Play: start track(); Pause / Stop: send it to the drive;
    // Silence: the drive is idle and nothing is coming, let it sleep.
    Action tick(bool deviceIdle) {
        if (m_phase == Phase::Playing && (m_stopAfterStart || m_pendingTrack >= 0)) {
            Action a = m_stopAfterStart ? Action::Stop : Action::Pause;
            m_stopAfterStart = false;
            m_phase = Phase::Stopping;
            return a;
        }
        if (m_phase != Phase::Idle || !deviceIdle) return Action::None;
        if (m_pendingTrack >= 0) {
            begin(m_pendingTrack, m_pendingLoop);
            m_pendingTrack = -1;
            return Action::Play;
        }
        if (m_silence) {
            m_silence = false;
            return Action::Silence;
        }
        return Action::None;
    }

    Phase phase() const { return m_phase; }
    bool playing() const { return m_phase == Phase::Playing; }
    bool looping() const { return m_loop; }
    int track() const { return m_track; }

  private:
    void begin(int track, bool loop) {
        m_phase = Phase::Starting;
        m_track = track;
        m_loop = loop;
        m_silence = false;
    }

    Phase m_phase = Phase::Idle;
    int m_track = -1;  // playing, or paused for a resume
    bool m_loop = false;
    int m_pendingTrack = -1;
    bool m_pendingLoop = false;
    bool m_stopAfterStart = false;
    bool m_silence = false;  // silence the drive once it is idle
};

}  // namespace psxsplash
