#pragma once

#include <psyqo-lua/lua.hh>
#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>

namespace psxsplash {

class SceneManager;  // Forward declaration
class CutscenePlayer;  // Forward declaration
class AnimationPlayer;  // Forward declaration
class UISystem;  // Forward declaration

/**
 * Lua API - Provides game scripting functionality
 * 
 * Available namespaces:
 * - Entity: Object finding, spawning, destruction
 * - Vec3: Vector math operations  
 * - Input: Controller state queries
 * - Timer: Timer control
 * - Camera: Camera manipulation
 * - Audio: Sound playback (future)
 * - Scene: Scene management
 */
class LuaAPI {
public:
    // Initialize all API modules
    static void RegisterAll(psyqo::Lua& L, SceneManager* scene, CutscenePlayer* cutscenePlayer = nullptr, AnimationPlayer* animationPlayer = nullptr, UISystem* uiSystem = nullptr);
    
    // Called once per frame to advance the Lua frame counter
    static void IncrementFrameCount();
    
    // Reset frame counter (called on scene load)
    static void ResetFrameCount();
    
private:
    // Store scene manager for API access
    static SceneManager* s_sceneManager;
    
    // Cutscene player pointer (set during RegisterAll)
    static CutscenePlayer* s_cutscenePlayer;

    // Animation player pointer (set during RegisterAll)
    static AnimationPlayer* s_animationPlayer;
    
    // UI system pointer (set during RegisterAll)
    static UISystem* s_uiSystem;
    
    // ========================================================================
    // ENTITY API
    // ========================================================================
    
    // Entity.FindByScriptIndex(index) -> object or nil
    // Finds first object with matching Lua script file index
    static int Entity_FindByScriptIndex(lua_State* L);
    
    // Entity.FindByIndex(index) -> object or nil
    // Gets object by its array index
    static int Entity_FindByIndex(lua_State* L);
    
    // Entity.Find(name) -> object or nil
    // Finds first object with matching name (user-friendly)
    static int Entity_Find(lua_State* L);
    
    // Entity.GetCount() -> number
    // Returns total number of game objects
    static int Entity_GetCount(lua_State* L);
    
    // Entity.SetActive(object, active)
    // Sets object active state (fires onEnable/onDisable)
    static int Entity_SetActive(lua_State* L);
    
    // Entity.IsActive(object) -> boolean
    static int Entity_IsActive(lua_State* L);
    
    // Entity.GetPosition(object) -> {x, y, z}
    static int Entity_GetPosition(lua_State* L);
    
    // Entity.SetPosition(object, {x, y, z})
    static int Entity_SetPosition(lua_State* L);
    
    // Entity.GetRotationY(object) -> number (radians)
    static int Entity_GetRotationY(lua_State* L);
    
    // Entity.SetRotationY(object, angle) -> nil
    static int Entity_SetRotationY(lua_State* L);
    
    // Entity.ForEach(callback) -> nil
    // Calls callback(object, index) for each active game object
    static int Entity_ForEach(lua_State* L);
    
    // ========================================================================
    // VEC3 API - Vector math
    // ========================================================================
    
    // Vec3.new(x, y, z) -> {x, y, z}
    static int Vec3_New(lua_State* L);
    
    // Vec3.add(a, b) -> {x, y, z}
    static int Vec3_Add(lua_State* L);
    
    // Vec3.sub(a, b) -> {x, y, z}
    static int Vec3_Sub(lua_State* L);
    
    // Vec3.mul(v, scalar) -> {x, y, z}
    static int Vec3_Mul(lua_State* L);
    
    // Vec3.dot(a, b) -> number
    static int Vec3_Dot(lua_State* L);
    
    // Vec3.cross(a, b) -> {x, y, z}
    static int Vec3_Cross(lua_State* L);
    
    // Vec3.length(v) -> number  
    static int Vec3_Length(lua_State* L);
    
    // Vec3.lengthSq(v) -> number (faster, no sqrt)
    static int Vec3_LengthSq(lua_State* L);
    
    // Vec3.normalize(v) -> {x, y, z}
    static int Vec3_Normalize(lua_State* L);
    
    // Vec3.distance(a, b) -> number
    static int Vec3_Distance(lua_State* L);
    
    // Vec3.distanceSq(a, b) -> number (faster)
    static int Vec3_DistanceSq(lua_State* L);
    
    // Vec3.lerp(a, b, t) -> {x, y, z}
    static int Vec3_Lerp(lua_State* L);
    
    // ========================================================================
    // INPUT API - Controller state
    // ========================================================================
    
    // Input.IsPressed(button) -> boolean
    // True only on the frame the button was pressed
    static int Input_IsPressed(lua_State* L);
    
    // Input.IsReleased(button) -> boolean
    // True only on the frame the button was released
    static int Input_IsReleased(lua_State* L);
    
    // Input.IsHeld(button) -> boolean
    // True while the button is held down
    static int Input_IsHeld(lua_State* L);
    
    // Input.GetAnalog(stick) -> x, y
    // Returns analog stick values (-128 to 127)
    static int Input_GetAnalog(lua_State* L);
    
    // Button constants (registered as Input.CROSS, Input.CIRCLE, etc.)
    static void RegisterInputConstants(psyqo::Lua& L);
    
    // ========================================================================
    // TIMER API - Frame counter
    // ========================================================================
    
    // Timer.GetFrameCount() -> number
    // Returns total frames since scene start
    static int Timer_GetFrameCount(lua_State* L);
    
    // ========================================================================
    // CAMERA API - Camera control
    // ========================================================================
    
    // Camera.GetPosition() -> {x, y, z}
    static int Camera_GetPosition(lua_State* L);
    
    // Camera.SetPosition(x, y, z)
    static int Camera_SetPosition(lua_State* L);
    
    // Camera.GetRotation() -> {x, y, z}
    static int Camera_GetRotation(lua_State* L);
    
    // Camera.SetRotation(x, y, z)
    static int Camera_SetRotation(lua_State* L);
    
    // Camera.GetForward() 
    static int Camera_GetForward(lua_State* L);

    // Camera.MoveForward(step) 
    static int Camera_MoveForward(lua_State* L);

    // Camera.MoveBackward(step) 
    static int Camera_MoveBackward(lua_State* L);

    // Camera.MoveLeft(step) 
    static int Camera_MoveLeft(lua_State* L);

    // Camera.MoveRight(step) 
    static int Camera_MoveRight(lua_State* L);

    // Camera.FollowPsxPlayer 
    static int Camera_FollowPsxPlayer(lua_State* L);

    // Camera.LookAt(target) or Camera.LookAt(x, y, z)
    static int Camera_LookAt(lua_State* L);

    // Camera.GetH() -> number (current projection H register value)
    static int Camera_GetH(lua_State* L);

    // Camera.SetH(h) -> nil (set projection H register, clamped 1-1024)
    static int Camera_SetH(lua_State* L);
    
    // ========================================================================
    // AUDIO API - Sound playback (placeholder for SPU)
    // ========================================================================
    
    // Audio.Play(soundId, volume, pan) -> channelId
    // soundId can be a number (clip index) or string (clip name)
    static int Audio_Play(lua_State* L);
    
    // Audio.Find(name) -> clipIndex or nil
    // Finds audio clip by name, returns its index for use with Play/Stop/etc.
    static int Audio_Find(lua_State* L);
    
    // Audio.Stop(channelId)
    static int Audio_Stop(lua_State* L);
    
    // Audio.SetVolume(channelId, volume)
    static int Audio_SetVolume(lua_State* L);
    
    // Audio.StopAll()
    static int Audio_StopAll(lua_State* L);
    
    // ========================================================================
    // DEBUG API - Development helpers
    // ========================================================================
    
    // Debug.Log(message)
    static int Debug_Log(lua_State* L);
    
    // Debug.DrawLine(start, end, color) - draws debug line next frame
    static int Debug_DrawLine(lua_State* L);
    
    // Debug.DrawBox(center, size, color)
    static int Debug_DrawBox(lua_State* L);
    
    // ========================================================================
    // MATH API - Additional math functions
    // ========================================================================
    
    // Math.Clamp(value, min, max)
    static int Math_Clamp(lua_State* L);
    
    // Math.Lerp(a, b, t)
    static int Math_Lerp(lua_State* L);
    
    // Math.Sign(value)
    static int Math_Sign(lua_State* L);
    
    // Math.Abs(value)
    static int Math_Abs(lua_State* L);
    
    // Math.Min(a, b)
    static int Math_Min(lua_State* L);
    
    // Math.Max(a, b)  
    static int Math_Max(lua_State* L);
    
    // ========================================================================
    // RANDOM API - Get random numbers
    // ========================================================================

    // Random.Number(max) returns from 1 to max inclusive
    static int Random_Number(lua_State* L);

    // Random.GeneratorNumber(max) returns from 1 to max inclusive
    static int Random_GeneratorNumber(lua_State* L);

    // Random.Range(min,max) returns from min inclusive to max inclusive 
    static int Random_Range(lua_State* L);

    // Random.GeneratorRange(min,max) returns from min inclusive to max inclusive
    static int Random_GeneratorRange(lua_State* L);

    // Random.Seed(newSeed) sets the seed for the random number generator 
    static int Random_GeneratorSeed(lua_State* L);

    // ========================================================================
    // SCENE API - Scene management
    // ========================================================================
    
    // Scene.Load(sceneIndex)
    // Requests a scene transition to the given index (0-based).
    // The actual load happens at the end of the current frame.
    static int Scene_Load(lua_State* L);
    
    // Scene.GetIndex() -> number
    // Returns the index of the currently loaded scene.
    static int Scene_GetIndex(lua_State* L);
    
    // ========================================================================
    // PERSIST API - Data that survives scene loads
    // ========================================================================
    
    // Persist.Get(key) -> number or nil
    static int Persist_Get(lua_State* L);
    
    // Persist.Set(key, value)
    static int Persist_Set(lua_State* L);
    
    // Reset all persistent data
    static void PersistClear();
    
    // ========================================================================
    // CUTSCENE API - Cutscene playback control
    // ========================================================================
    
    // Cutscene.Play(name) or Cutscene.Play(name, {loop=bool, onComplete=fn})
    static int Cutscene_Play(lua_State* L);

    // Cutscene.Stop() -> nil
    static int Cutscene_Stop(lua_State* L);

    // Cutscene.IsPlaying() -> boolean
    static int Cutscene_IsPlaying(lua_State* L);

    // ========================================================================
    // ANIMATION API - Multi-instance animation playback
    // ========================================================================

    // Animation.Play(name) or Animation.Play(name, {loop=bool, onComplete=fn})
    static int Animation_Play(lua_State* L);

    // Animation.Stop(name) -> nil
    static int Animation_Stop(lua_State* L);

    // Animation.IsPlaying(name) -> boolean
    static int Animation_IsPlaying(lua_State* L);

    // ========================================================================
    // SKINNED ANIMATION API - Bone-based mesh animation
    // ========================================================================

    // SkinnedAnim.Play(objectName, clipName) or (objectName, clipName, {loop, onComplete})
    static int SkinnedAnim_Play(lua_State* L);

    // SkinnedAnim.Stop(objectName) -> nil
    static int SkinnedAnim_Stop(lua_State* L);

    // SkinnedAnim.IsPlaying(objectName) -> boolean
    static int SkinnedAnim_IsPlaying(lua_State* L);

    // SkinnedAnim.GetClip(objectName) -> string or nil
    static int SkinnedAnim_GetClip(lua_State* L);

    // Controls.SetEnabled(bool) - enable/disable all player input
    static int Controls_SetEnabled(lua_State* L);

    // Controls.IsEnabled() -> boolean
    static int Controls_IsEnabled(lua_State* L);

    // Interact.SetEnabled(entity, bool) - enable/disable interaction + prompt for an object
    static int Interact_SetEnabled(lua_State* L);

    // Interact.IsEnabled(entity) -> boolean
    static int Interact_IsEnabled(lua_State* L);

    // ========================================================================
    // UI API - Canvas and element control
    // ========================================================================
    
    static int UI_FindCanvas(lua_State* L);
    static int UI_SetCanvasVisible(lua_State* L);
    static int UI_IsCanvasVisible(lua_State* L);
    static int UI_FindElement(lua_State* L);
    static int UI_SetVisible(lua_State* L);
    static int UI_IsVisible(lua_State* L);
    static int UI_SetText(lua_State* L);
    static int UI_GetText(lua_State* L);
    static int UI_SetProgress(lua_State* L);
    static int UI_GetProgress(lua_State* L);
    static int UI_SetColor(lua_State* L);
    static int UI_GetColor(lua_State* L);
    static int UI_SetPosition(lua_State* L);
    static int UI_GetPosition(lua_State* L);
    static int UI_SetSize(lua_State* L);
    static int UI_GetSize(lua_State* L);
    static int UI_SetProgressColors(lua_State* L);
    static int UI_GetElementType(lua_State* L);
    static int UI_GetElementCount(lua_State* L);
    static int UI_GetElementByIndex(lua_State* L);
    
    // ========================================================================
    // PLAYER API - Controlling the PsxPlayer
    // ========================================================================
    
    static int Player_SetPosition(lua_State* L);
    static int Player_GetPosition(lua_State* L);
    static int Player_SetRotation(lua_State* L);
    static int Player_GetRotation(lua_State* L);
    
    // ========================================================================
    // HELPERS
    // ========================================================================
    
    // Push a Vec3 table onto the stack
    static void PushVec3(psyqo::Lua& L, psyqo::FixedPoint<12> x, 
                         psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);
    
    // Read a Vec3 table from the stack
    static void ReadVec3(psyqo::Lua& L, int idx, 
                         psyqo::FixedPoint<12>& x, 
                         psyqo::FixedPoint<12>& y, 
                         psyqo::FixedPoint<12>& z);
};

}  // namespace psxsplash
