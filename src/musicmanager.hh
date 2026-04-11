#pragma once

#include <psyqo/cdrom-device.hh>

namespace psxsplash {

class MusicManager {
public:
    MusicManager();

    // Play a CD-DA track
    void playCDDATrack(int trackNum);
    void resumeCDDA();
    void pauseCDDA();
    void stopCDDA();

    void setCDRomDevice(psyqo::CDRomDevice* device) { mCDRomDevice = device; }

    struct CDDATrackReport {
        unsigned int position;
        unsigned int length;
        int trackNumber;
    };
private:
    psyqo::CDRomDevice* mCDRomDevice = nullptr;
    bool mPlayingCDDA = false;
    int mCurrentCDDATrack = -1;
};

} // namespace psxsplash
