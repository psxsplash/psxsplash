#include "musicmanager.hh"
#include "cdromhelper.hh"

#include <common/syscalls/syscalls.h>
#include <psyqo/spu.hh>

#include <common/hardware/spu.h>

namespace psxsplash {
MusicManager::MusicManager() {
}

void MusicManager::playCDDATrack(int trackNum) {
    //CDRomHelper::WakeDrive();

    if (!(SPU_CTRL & 0x1)) {
        SPU_CTRL |= 0x1;
        SPU_VOL_CD_LEFT = 0x7fff;
        SPU_VOL_CD_RIGHT = 0x7fff;
    }

    mCDRomDevice->playCDDATrack(trackNum,
        [this, trackNum](bool success) {
            mPlayingCDDA = success;
            ramsyscall_printf("CDDA playback success %d\n", success);
            if (mPlayingCDDA)
                mCurrentCDDATrack = trackNum;
    });
}

void MusicManager::resumeCDDA() {
    ramsyscall_printf("resume\n");
    if (mCurrentCDDATrack >= 0 && !mPlayingCDDA) {
        mCDRomDevice->resumeCDDA([this](bool success) {
            if (success)
                mPlayingCDDA = true;
        });
    }
}

void MusicManager::pauseCDDA() {
    ramsyscall_printf("pause\n");
    if (mCurrentCDDATrack >= 0 && mPlayingCDDA) {
        mCDRomDevice->pauseCDDA();
        mPlayingCDDA = false;
    }
}

void MusicManager::stopCDDA() {
    mCDRomDevice->stopCDDA();
    mCurrentCDDATrack = -1;
    mPlayingCDDA = false;
}

} // namespace psxsplash