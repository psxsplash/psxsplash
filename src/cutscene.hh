#pragma once

#include <stdint.h>
#include <psyqo/fixed-point.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/soft-math.hh>

#include "camera.hh"
#include "gameobject.hh"
#include "audiomanager.hh"
#include "controls.hh"

#include <psyqo-lua/lua.hh>

namespace psxsplash {

class UISystem;  // Forward declaration

// Forward declarations for skinned mesh animation triggers
struct SkinAnimState;
class SceneManager;

static constexpr int MAX_CUTSCENES         = 16;
static constexpr int MAX_TRACKS            = 8;
static constexpr int MAX_KEYFRAMES         = 64;
static constexpr int MAX_AUDIO_EVENTS      = 64;
static constexpr int MAX_SKIN_ANIM_EVENTS  = 16;

enum class TrackType : uint8_t {
    CameraPosition  = 0,
    CameraRotation  = 1,
    ObjectPosition  = 2,
    ObjectRotation  = 3,
    ObjectActive    = 4,
    UICanvasVisible = 5, 
    UIElementVisible= 6,   
    UIProgress      = 7,   
    UIPosition      = 8,   
    UIColor         = 9,  
    CameraH         = 10,
    RumbleSmall     = 11,
    RumbleLarge     = 12,
};

/// Helper: true if a TrackType drives a UI property (canvas or element).
inline bool isUITrackType(TrackType t) {
    uint8_t v = static_cast<uint8_t>(t);
    return v >= 5 && v <= 9;
}

/// Helper: true if a TrackType drives a vibration motor.
inline bool isVibrationTrackType(TrackType t) {
    return t == TrackType::RumbleSmall || t == TrackType::RumbleLarge;
}

enum class InterpMode : uint8_t {
    Linear    = 0, 
    Step      = 1, 
    EaseIn    = 2, 
    EaseOut   = 3, 
    EaseInOut = 4, 
};

struct CutsceneKeyframe {
    // Upper 3 bits = InterpMode (0-7), lower 13 bits = frame number (0-8191).
    // At 30fps, max frame 8191 ≈ 4.5 minutes.
    uint16_t frameAndInterp;
    int16_t  values[3];

    uint16_t getFrame() const { return frameAndInterp & 0x1FFF; }
    InterpMode getInterp() const { return static_cast<InterpMode>(frameAndInterp >> 13); }
};
static_assert(sizeof(CutsceneKeyframe) == 8, "CutsceneKeyframe must be 8 bytes");

struct CutsceneAudioEvent {
    uint16_t frame;
    uint8_t  clipIndex;
    uint8_t  volume;
    uint8_t  pan;
    uint8_t  pad[3];
};
static_assert(sizeof(CutsceneAudioEvent) == 8, "CutsceneAudioEvent must be 8 bytes");

/// Skinned-mesh animation trigger within a cutscene or animation.
struct CutsceneSkinAnimEvent {
    uint16_t frame;         // Trigger frame
    uint8_t  skinMeshIndex; // Index into SceneManager::m_skinAnimSets
    uint8_t  clipIndex;     // Index into the skinned mesh's clips
    uint8_t  loop;          // 0 = one-shot, 1 = looping
    uint8_t  pad[3];
};
static_assert(sizeof(CutsceneSkinAnimEvent) == 8, "CutsceneSkinAnimEvent must be 8 bytes");

struct CutsceneTrack {
    TrackType         trackType;
    uint8_t           keyframeCount;
    uint8_t           pad[2];
    CutsceneKeyframe* keyframes;  
    GameObject*       target; 
    int16_t uiHandle;
    int16_t initialValues[3];
};

struct Cutscene {
    const char*              name;        // Points into splashpack data
    uint16_t                 totalFrames;
    uint8_t                  trackCount;
    uint8_t                  audioEventCount;
    uint8_t                  skinAnimEventCount;
    uint8_t                  _pad[3];
    CutsceneTrack            tracks[MAX_TRACKS];
    CutsceneAudioEvent*      audioEvents;     // Points into splashpack data
    CutsceneSkinAnimEvent*   skinAnimEvents;   // Points into splashpack data
};


class CutscenePlayer {
public:
    /// Initialize with loaded cutscene data. Safe to pass nullptr/0 if no cutscenes.
    void init(Cutscene* cutscenes, int count, Camera* camera, AudioManager* audio,
              UISystem* uiSystem = nullptr, SceneManager* sceneMgr = nullptr,
              Controls* controls = nullptr);

    /// Play cutscene by name. Returns false if not found.
    /// If loop is true, the cutscene replays from the start when it ends.
    bool play(const char* name, bool loop = false);

    /// Stop the current cutscene immediately.
    void stop();

    /// True if a cutscene is currently active.
    bool isPlaying() const { return m_active != nullptr; }

    /// True if the active cutscene has camera tracks (position or rotation).
    /// Use this to decide whether to suppress player camera follow.
    bool hasCameraTracks() const;

    /// Set a Lua registry reference to call when the cutscene finishes.
    /// Pass LUA_NOREF to clear. The callback is called ONCE when the
    /// cutscene ends (not on each loop iteration - only when it truly stops).
    void setOnCompleteRef(int ref) { m_onCompleteRef = ref; }
    int  getOnCompleteRef() const  { return m_onCompleteRef; }

    /// Set the lua_State for callbacks. Must be called before play().
    void setLuaState(lua_State* L) { m_luaState = L; }

    /// Advance by dt12 (time-based). dt12 is in 0.12 fixed-point
    /// (4096 = one 30fps frame). Call once per frame.
    void tick(int32_t dt12);

private:
    Cutscene*      m_cutscenes  = nullptr;
    int            m_count      = 0;
    Cutscene*      m_active     = nullptr;
    uint16_t       m_frame      = 0;       // Current whole frame
    uint16_t       m_subFrame   = 0;       // 0..4095 (0.12 fp fraction within frame)
    uint8_t        m_nextAudio  = 0;
    uint8_t        m_nextSkinAnim = 0;
    bool           m_loop       = false;
    Camera*        m_camera     = nullptr;
    AudioManager*  m_audio      = nullptr;
    UISystem*      m_uiSystem   = nullptr;
    SceneManager*  m_sceneMgr   = nullptr;
    Controls*      m_controls   = nullptr;
    lua_State*     m_luaState   = nullptr;
    int            m_onCompleteRef = LUA_NOREF;

    psyqo::Trig<> m_trig;

    void applyTrack(CutsceneTrack& track);
    void fireOnComplete();
};

}  // namespace psxsplash
