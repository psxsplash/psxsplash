#include "musicmanager.hh"
#include "cdromhelper.hh"

#include <common/syscalls/syscalls.h>
#include <psyqo/fixed-point.hh>
#include <psyqo/spu.hh>

#include <common/hardware/spu.h>

#include "lstate.h"

namespace psxsplash {
MusicManager::MusicManager() {
}

void MusicManager::playCDDATrack(int trackNum) {
#ifdef LOADER_CDROM
    CDRomHelper::WakeDrive();

    if (!(SPU_CTRL & 0x1)) {
        SPU_CTRL |= 0x1;
        SPU_VOL_CD_LEFT = 0x7fff;
        SPU_VOL_CD_RIGHT = 0x7fff;
    }

    mCDRomDevice->playCDDATrack(trackNum,
        [this, trackNum](bool success) {
            mPlayingCDDA = success;
            if (mPlayingCDDA)
                mCurrentCDDATrack = trackNum;
    });
#endif // LOADER_CDROM
}

void MusicManager::resumeCDDA() {
#ifdef LOADER_CDROM
    if (mCurrentCDDATrack >= 0 && !mPlayingCDDA) {
        mCDRomDevice->resumeCDDA([this](bool success) {
            if (success)
                mPlayingCDDA = true;
        });
    }
#endif // LOADER_CDROM
}

void MusicManager::pauseCDDA() {
#ifdef LOADER_CDROM
    if (mCurrentCDDATrack >= 0 && mPlayingCDDA) {
        mCDRomDevice->pauseCDDA();
        mPlayingCDDA = false;
    }
#endif // LOADER_CDROM
}

void MusicManager::stopCDDA() {
#ifdef LOADER_CDROM
    mCDRomDevice->stopCDDA();
    mCurrentCDDATrack = -1;
    mPlayingCDDA = false;
#endif // LOADER_CDROM
}

void MusicManager::tellCDDA(lua_State* L) {
#ifdef LOADER_CDROM
    if (!L) return;
    psyqo::Lua luaState(L);
    int cb = LUA_NOREF;
    if (luaState.isFunction(-1)) {
        cb = luaState.ref();
    }
    else {
        luaState.pop();
    }

    mCDRomDevice->getPlaybackLocation([L, cb](psyqo::CDRomDevice::PlaybackLocation* location) {
        if (location && cb != LUA_NOREF) {
            psyqo::FixedPoint<12> loc((location->relative.m * 60) + location->relative.s,location->relative.f * 54);

            psyqo::Lua luaState(L);
            luaState.rawGetI(LUA_REGISTRYINDEX, cb);
            if (luaState.isFunction(-1)) {
                luaState.push(loc);
                if (luaState.pcall(1, 0) != LUA_OK) {
                    luaState.pop();
                    luaState.pop();
                }
            } else {
                luaState.pop();
            }
        }
    });
#endif // LOADER_CDROM
}

void MusicManager::setCDDAVolume(int left, int right) {
#ifdef LOADER_CDROM
    mCDRomDevice->setVolume(left, 0, 0, right);
#endif // LOADER_CDROM
}
} // namespace psxsplash