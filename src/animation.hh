#pragma once

#include <stdint.h>
#include <psyqo/trigonometry.hh>

#include "cutscene.hh"
#include <psyqo-lua/lua.hh>

namespace psxsplash {

class UISystem;

static constexpr int MAX_ANIMATIONS          = 16;
static constexpr int MAX_ANIM_TRACKS         = 8;
static constexpr int MAX_SIMULTANEOUS_ANIMS  = 8;

struct Animation {
    const char*      name;
    uint16_t         totalFrames;
    uint8_t          trackCount;
    uint8_t          pad;
    CutsceneTrack    tracks[MAX_ANIM_TRACKS];
};

class AnimationPlayer {
public:
    void init(Animation* animations, int count, UISystem* uiSystem = nullptr);

    /// Play animation by name. Returns false if not found or no free slots.
    bool play(const char* name, bool loop = false);

    /// Stop all instances of animation by name.
    void stop(const char* name);

    /// Stop all running animations.
    void stopAll();

    /// True if any instance of the named animation is playing.
    bool isPlaying(const char* name) const;

    /// Set a Lua callback for the next play() call.
    void setOnCompleteRef(const char* name, int ref);

    /// Set the lua_State for callbacks.
    void setLuaState(lua_State* L) { m_luaState = L; }

    /// Advance all active animations one frame.
    void tick();

private:
    struct ActiveSlot {
        Animation* anim     = nullptr;
        uint16_t   frame    = 0;
        bool       loop     = false;
        int        onCompleteRef = LUA_NOREF;
    };

    Animation*    m_animations  = nullptr;
    int           m_animCount   = 0;
    ActiveSlot    m_slots[MAX_SIMULTANEOUS_ANIMS];
    UISystem*     m_uiSystem    = nullptr;
    lua_State*    m_luaState    = nullptr;
    psyqo::Trig<> m_trig;

    Animation* findByName(const char* name) const;
    void applyTrack(CutsceneTrack& track, uint16_t frame);
    void captureInitialValues(Animation* anim);
    void fireSlotComplete(ActiveSlot& slot);
};

}  // namespace psxsplash
