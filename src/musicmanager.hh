#pragma once

#include <psyqo/cdrom-device.hh>
#include <psyqo-lua/lua.hh>

#include "cddastate.hh"

namespace psxsplash {

class MusicManager {
public:
    MusicManager();

    // CD-DA functions
    // loop: when the track runs out, play it again, until a pause, a stop,
    // another play, a scene load or a movie.
    void playCDDATrack(int trackNum, bool loop = false);
    void resumeCDDA();
    void pauseCDDA();
    void stopCDDA();
    void tellCDDA(lua_State* L);
    void setCDDAVolume(int left, int right);
    bool isPlayingCDDA() const { return mState.playing(); }

    // The drive is wanted for a scene load or a movie: no loop comes round
    // again and no queued play starts. A playing track is the caller's to stop.
    void cancelLoop() { mState.cancel(); }

    // Per frame: starts a looping track again, or a play that had to wait for
    // the drive, and once a stopped (or finished) track leaves the CD-ROM
    // device idle, silences the drive. psyqo's stopCDDA() has no completion
    // callback.
    void tick();

    void setCDRomDevice(psyqo::CDRomDevice* device) { mCDRomDevice = device; }
private:
    void startTrack();

    psyqo::CDRomDevice* mCDRomDevice = nullptr;
    CDDAState mState;
};

} // namespace psxsplash
