#include "musicmanager.hh"
#include "cdromhelper.hh"
#include "features.hh"
#include "streamreader.hh"

#include <common/syscalls/syscalls.h>
#include <psyqo/fixed-point.hh>
#include <psyqo/spu.hh>
#include <psyqo/xprintf.h>

#include <common/hardware/spu.h>

#include "lstate.h"

namespace psxsplash {
MusicManager::MusicManager() {
}

void MusicManager::playCDDATrack(int trackNum) {
    if (!mCDRomDevice) return;  // archive image read over PCdrv: no drive, no CD-DA
    // A streaming scene needs the drive for data: CD-DA and streaming are
    // mutually exclusive, so such a scene uses sequenced music instead.
#if PSXSPLASH_FEATURE_STREAMING
    if (StreamReader::Get().driveReserved()) {
        printf("Audio.PlayCDDA ignored: this scene streams from the CD\n");
        return;
    }
#endif
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
}

void MusicManager::resumeCDDA() {
    if (!mCDRomDevice) return;
#if PSXSPLASH_FEATURE_STREAMING
    if (StreamReader::Get().driveReserved()) return;
#endif
    if (mCurrentCDDATrack >= 0 && !mPlayingCDDA) {
        mCDRomDevice->resumeCDDA([this](bool success) {
            if (success)
                mPlayingCDDA = true;
        });
    }
}

void MusicManager::pauseCDDA() {
    if (!mCDRomDevice) return;
    if (mCurrentCDDATrack >= 0 && mPlayingCDDA) {
        mCDRomDevice->pauseCDDA();
        mPlayingCDDA = false;
    }
}

void MusicManager::stopCDDA() {
    if (!mCDRomDevice) return;
    mCDRomDevice->stopCDDA();
    mCurrentCDDATrack = -1;
    mPlayingCDDA = false;
}

void MusicManager::tellCDDA(lua_State* L) {
    if (!mCDRomDevice) return;
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
        if (cb == LUA_NOREF) return;
        psyqo::Lua luaState(L);
        if (location) {
            psyqo::FixedPoint<12> loc((location->relative.m * 60) + location->relative.s,location->relative.f * 54);

            luaState.rawGetI(LUA_REGISTRYINDEX, cb);
            if (luaState.isFunction(-1)) {
                luaState.push(loc);
                // A failed pcall leaves exactly one value, the error message.
                if (luaState.pcall(1, 0) != LUA_OK) {
                    luaState.pop();
                }
            } else {
                luaState.pop();
            }
        }
        // The callback runs exactly once on every path, including the immediate
        // null-location one, so this is the only place the registry slot can be
        // released. Releasing it only alongside a valid location leaks one slot
        // per call whenever no CD action was in progress.
        luaState.unref(LUA_REGISTRYINDEX, cb);
    });
}

void MusicManager::setCDDAVolume(int left, int right) {
    if (!mCDRomDevice) return;
    mCDRomDevice->setVolume(left, 0, 0, right);
}
} // namespace psxsplash