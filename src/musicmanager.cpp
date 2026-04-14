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
    CDRomHelper::WakeDrive();

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

void MusicManager::tellCDDA(lua_State* L) {
    if (!L) return;
    psyqo::Lua luaState(L);
    int cb = LUA_NOREF;
    if (luaState.isFunction(-1)) {
        ramsyscall_printf("function\n");
        cb = luaState.ref();
    }
    else {
        luaState.pop();
    }

    mCDRomDevice->getPlaybackLocation([L, cb](psyqo::CDRomDevice::PlaybackLocation* location) {
        ramsyscall_printf("cb %d\n", cb);
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
}

void MusicManager::setCDDAVolume(int left, int right) {
    mCDRomDevice->setVolume(left, 0, 0, right);
}
} // namespace psxsplash