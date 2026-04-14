#pragma once

#include <psyqo/cdrom-device.hh>
#include <psyqo-lua/lua.hh>

namespace psxsplash {

class MusicManager {
public:
    MusicManager();

    // CD-DA functions
    void playCDDATrack(int trackNum);
    void resumeCDDA();
    void pauseCDDA();
    void stopCDDA();
    void tellCDDA(lua_State* L);
    void setCDDAVolume(int left, int right);
    bool isPlayingCDDA() const { return mPlayingCDDA; }

    void setCDRomDevice(psyqo::CDRomDevice* device) { mCDRomDevice = device; }
private:
    psyqo::CDRomDevice* mCDRomDevice = nullptr;
    bool mPlayingCDDA = false;
    int mCurrentCDDATrack = -1;
};

} // namespace psxsplash
