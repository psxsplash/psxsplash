#include "luaapi.hh"
#include "memorycardmanager.hh"
#include "scenemanager.hh"
#include "gameobject.hh"
#include "controls.hh"
#include "camera.hh"
#include "cutscene.hh"
#include "animation.hh"
#include "skinmesh.hh"
#include "sio1.hh"
#include "spritesystem.hh"
#include "tilesystem.hh"
#include "uisystem.hh"
#include "networkmanager.hh"

#include "renderer.hh"

#include <psyqo/atan2.hh>
#include <psyqo/gte-math.hh>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/fixed-point.hh>
#include "gtemath.hh"
#include "luautility.hh"

#include <psyqo/xprintf.h>

namespace psxsplash {

// Static member
SceneManager* LuaAPI::s_sceneManager = nullptr;
CutscenePlayer* LuaAPI::s_cutscenePlayer = nullptr;
AnimationPlayer* LuaAPI::s_animationPlayer = nullptr;
UISystem* LuaAPI::s_uiSystem = nullptr;
SpriteSystem* LuaAPI::s_spriteSystem = nullptr;
TileSystem* LuaAPI::s_tileSystem = nullptr;

// Scale factor: FixedPoint<12> stores 1.0 as raw 4096.
// Lua scripts work in world-space units (1 = one unit), so we convert.
static constexpr lua_Number kFixedScale = 4096;

// Read a FixedPoint<12> from the stack, accepting either a FixedPoint object
// or a plain integer (which gets scaled by 4096 to become fp12).
static psyqo::FixedPoint<12> readFP(psyqo::Lua& L, int idx) {
    if (IsFixedPointSafe(L, idx)) {
        return L.toFixedPoint(idx);
    }
    return psyqo::FixedPoint<12>(static_cast<int32_t>(L.toNumber(idx) * kFixedScale), psyqo::FixedPoint<12>::RAW);
}

// Angle scale: psyqo::Angle is FixedPoint<10>, so 1.0_pi = raw 1024
static constexpr lua_Number kAngleScale = 1024;
static psyqo::Trig<> s_trig;

// ============================================================================
// REGISTRATION
// ============================================================================

void LuaAPI::RegisterAll(psyqo::Lua& L, SceneManager* scene, CutscenePlayer* cutscenePlayer, AnimationPlayer* animationPlayer, UISystem* uiSystem, SpriteSystem* spriteSystem, TileSystem* tileSystem) {
    s_sceneManager = scene;
    s_cutscenePlayer = cutscenePlayer;
    s_animationPlayer = animationPlayer;
    s_uiSystem = uiSystem;
    s_spriteSystem = spriteSystem;
    s_tileSystem = tileSystem;
    
    // ========================================================================
    // ACTOR API
    // ========================================================================
    L.newTable();  // Actor table

    L.push(Actor_GetPlayer);
    L.setField(-2, "GetPlayer");

    L.push(Actor_Find);
    L.setField(-2, "Find");

    L.push(Actor_FindByIndex);
    L.setField(-2, "FindByIndex");

    L.push(Actor_GetCount);
    L.setField(-2, "GetCount");

    L.push(Actor_IsPlayer);
    L.setField(-2, "IsPlayer");

    L.push(Actor_GetName);
    L.setField(-2, "GetName");

    L.push(Actor_GetPosition);
    L.setField(-2, "GetPosition");

    L.push(Actor_GetPositionXZ);
    L.setField(-2, "GetPositionXZ");

    L.push(Actor_SetPosition);
    L.setField(-2, "SetPosition");

    L.push(Actor_GetRotation);
    L.setField(-2, "GetRotation");

    L.push(Actor_SetRotation);
    L.setField(-2, "SetRotation");

    L.push(Actor_GetEntity);
    L.setField(-2, "GetEntity");

    L.push(Actor_GetNavRegion);
    L.setField(-2, "GetNavRegion");

    L.push(Actor_FindPath);
    L.setField(-2, "FindPath");

    L.setGlobal("Actor");

    // ========================================================================
    // AGENT API
    // ========================================================================
    L.newTable();

    L.push(Agent_IsAgent);
    L.setField(-2, "IsAgent");

    L.push(Agent_SetEnabled);
    L.setField(-2, "SetEnabled");

    L.push(Agent_IsEnabled);
    L.setField(-2, "IsEnabled");

    L.push(Agent_MoveTo);
    L.setField(-2, "MoveTo");

    L.push(Agent_SetTarget);
    L.setField(-2, "SetTarget");

    L.push(Agent_Stop);
    L.setField(-2, "Stop");

    L.push(Agent_IsMoving);
    L.setField(-2, "IsMoving");

    L.push(Agent_GetTarget);
    L.setField(-2, "GetTarget");

    L.push(Agent_SetSpeed);
    L.setField(-2, "SetSpeed");

    L.push(Agent_GetSpeed);
    L.setField(-2, "GetSpeed");

    // State machine
    L.push(Agent_GetState);
    L.setField(-2, "GetState");

    L.push(Agent_SetState);
    L.setField(-2, "SetState");

    // Vision / hearing
    L.push(Agent_CanSee);
    L.setField(-2, "CanSee");

    L.push(Agent_CanHear);
    L.setField(-2, "CanHear");

    L.push(Agent_SetVisionRange);
    L.setField(-2, "SetVisionRange");

    L.push(Agent_GetVisionRange);
    L.setField(-2, "GetVisionRange");

    L.push(Agent_SetVisionAngle);
    L.setField(-2, "SetVisionAngle");

    L.push(Agent_SetHearingRange);
    L.setField(-2, "SetHearingRange");

    L.push(Agent_GetHearingRange);
    L.setField(-2, "GetHearingRange");

    L.push(Agent_SetAlertTimeout);
    L.setField(-2, "SetAlertTimeout");

    L.push(Agent_GetLastKnownPos);
    L.setField(-2, "GetLastKnownPos");

    // Patrol waypoints
    L.push(Agent_AddWaypoint);
    L.setField(-2, "AddWaypoint");

    L.push(Agent_ClearWaypoints);
    L.setField(-2, "ClearWaypoints");

    L.push(Agent_SetPatrolEnabled);
    L.setField(-2, "SetPatrolEnabled");

    L.setGlobal("Agent");

    // ========================================================================
    // NET API (serial multiplayer over SIO1)
    // ========================================================================
    L.newTable();  // Net table

    L.push(Net_Connect);
    L.setField(-2, "Connect");
    L.push(Net_Disconnect);
    L.setField(-2, "Disconnect");
    L.push(Net_IsConnected);
    L.setField(-2, "IsConnected");
    L.push(Net_IsHost);
    L.setField(-2, "IsHost");
    L.push(Net_State);
    L.setField(-2, "State");
    L.push(Net_Stats);
    L.setField(-2, "Stats");
    L.push(Net_LocalSlot);
    L.setField(-2, "LocalSlot");
    L.push(Net_PlayerCount);
    L.setField(-2, "PlayerCount");
    L.push(Net_SetLocalAvatar);
    L.setField(-2, "SetLocalAvatar");
    L.push(Net_SetReplicationEnabled);
    L.setField(-2, "SetReplicationEnabled");
    L.push(Net_SetRemoteAvatar);
    L.setField(-2, "SetRemoteAvatar");
    L.push(Net_RegisterActor);
    L.setField(-2, "RegisterActor");
    L.push(Net_UnregisterActor);
    L.setField(-2, "UnregisterActor");
    L.push(Net_Send);
    L.setField(-2, "Send");
    L.push(Net_SyncActor);
    L.setField(-2, "SyncActor");
    L.push(Net_SendData);
    L.setField(-2, "SendData");
    L.push(Net_ReliableQueueDepth);
    L.setField(-2, "ReliableQueueDepth");
    L.push(Net_SetPersistent);
    L.setField(-2, "SetPersistent");
    L.push(Net_IsPersistent);
    L.setField(-2, "IsPersistent");

    L.setGlobal("Net");

    // ========================================================================
    // ENTITY API
    // ========================================================================
    L.newTable();  // Entity table
    
    L.push(Entity_FindByScriptIndex);
    L.setField(-2, "FindByScriptIndex");
    
    L.push(Entity_FindByIndex);
    L.setField(-2, "FindByIndex");
    
    L.push(Entity_Find);
    L.setField(-2, "Find");
    
    L.push(Entity_GetCount);
    L.setField(-2, "GetCount");
    
    L.push(Entity_SetActive);
    L.setField(-2, "SetActive");
    
    L.push(Entity_IsActive);
    L.setField(-2, "IsActive");
    
    L.push(Entity_GetPosition);
    L.setField(-2, "GetPosition");
    
    L.push(Entity_SetPosition);
    L.setField(-2, "SetPosition");
    
    L.push(Entity_GetRotationY);
    L.setField(-2, "GetRotationY");
    
    L.push(Entity_SetRotationY);
    L.setField(-2, "SetRotationY");

    L.push(Entity_SetRotation);
    L.setField(-2, "SetRotation");

    L.push(Entity_GetForward);
    L.setField(-2, "GetForward");

    L.push(Entity_GetRight);
    L.setField(-2, "GetRight");

    L.push(Entity_GetUp);
    L.setField(-2, "GetUp");

    L.push(Entity_MoveForward);
    L.setField(-2, "MoveForward");

    L.push(Entity_MoveBackward);
    L.setField(-2, "MoveBackward");

    L.push(Entity_MoveLeft);
    L.setField(-2, "MoveLeft");

    L.push(Entity_MoveRight);
    L.setField(-2, "MoveRight");

    L.push(Entity_MoveUp);
    L.setField(-2, "MoveUp");

    L.push(Entity_MoveDown);
    L.setField(-2, "MoveDown");
    
    L.push(Entity_ForEach);
    L.setField(-2, "ForEach");

    L.push(Entity_SetUVOffset);
    L.setField(-2, "SetUVOffset");
    
    L.push(Entity_SetUVs);
    L.setField(-2, "SetUVs");

    L.push(Entity_SetTPage);
    L.setField(-2, "SetTPage");

    L.push(Entity_SetParent);
    L.setField(-2, "SetParent");

    L.setGlobal("Entity");
    
    // ========================================================================
    // VEC3 API
    // ========================================================================
    L.newTable();  // Vec3 table
    
    L.push(Vec3_New);
    L.setField(-2, "new");
    
    L.push(Vec3_Add);
    L.setField(-2, "add");
    
    L.push(Vec3_Sub);
    L.setField(-2, "sub");
    
    L.push(Vec3_Mul);
    L.setField(-2, "mul");
    
    L.push(Vec3_Dot);
    L.setField(-2, "dot");
    
    L.push(Vec3_Cross);
    L.setField(-2, "cross");
    
    L.push(Vec3_Length);
    L.setField(-2, "length");
    
    L.push(Vec3_LengthSq);
    L.setField(-2, "lengthSq");
    
    L.push(Vec3_Normalize);
    L.setField(-2, "normalize");
    
    L.push(Vec3_Distance);
    L.setField(-2, "distance");
    
    L.push(Vec3_DistanceSq);
    L.setField(-2, "distanceSq");
    
    L.push(Vec3_Lerp);
    L.setField(-2, "lerp");
    
    L.setGlobal("Vec3");
    
    // ========================================================================
    // INPUT API
    // ========================================================================
    L.newTable();  // Input table
    
    L.push(Input_IsPressedPlayer1);
    L.setField(-2, "IsPressedPlayer1");
    
    L.push(Input_IsReleasedPlayer1);
    L.setField(-2, "IsReleasedPlayer1");
    
    L.push(Input_IsHeldPlayer1);
    L.setField(-2, "IsHeldPlayer1");
    
    L.push(Input_GetAnalogPlayer1);
    L.setField(-2, "GetAnalogPlayer1");
    
    L.push(Input_IsPressedPlayer2);
    L.setField(-2, "IsPressedPlayer2");

    L.push(Input_IsReleasedPlayer2);
    L.setField(-2, "IsReleasedPlayer2");

    L.push(Input_IsHeldPlayer2);
    L.setField(-2, "IsHeldPlayer2");

    L.push(Input_GetAnalogPlayer2);
    L.setField(-2, "GetAnalogPlayer2");

    L.push(Input_BindToActor);
    L.setField(-2, "BindToActor");
    L.push(Input_GetBoundActor);
    L.setField(-2, "GetBoundActor");

    // Register button constants
    RegisterInputConstants(L);
    
    L.setGlobal("Input");
    
    // ========================================================================
    // TIMER API
    // ========================================================================
    L.newTable();  // Timer table
    
    L.push(Timer_GetFrameCount);
    L.setField(-2, "GetFrameCount");
    
    L.setGlobal("Timer");
    
    // ========================================================================
    // CAMERA API
    // ========================================================================
    L.newTable();  // Camera table
    
    L.push(Camera_GetPosition);
    L.setField(-2, "GetPosition");
    
    L.push(Camera_SetPosition);
    L.setField(-2, "SetPosition");
    
    L.push(Camera_GetRotation);
    L.setField(-2, "GetRotation");
    
    L.push(Camera_SetRotation);
    L.setField(-2, "SetRotation");

    L.push(Camera_GetForward);
    L.setField(-2, "GetForward");

    L.push(Camera_MoveForward);
    L.setField(-2, "MoveForward");

    L.push(Camera_MoveBackward);
    L.setField(-2, "MoveBackward");

    L.push(Camera_MoveLeft);
    L.setField(-2, "MoveLeft");

    L.push(Camera_MoveRight);
    L.setField(-2, "MoveRight");
    
    L.push(Camera_FollowPsxPlayer);
    L.setField(-2, "FollowPsxPlayer");

    L.push(Camera_SetFollowTarget);
    L.setField(-2, "SetFollowTarget");

    L.push(Camera_GetFollowTarget);
    L.setField(-2, "GetFollowTarget");

    L.push(Camera_ClearFollowTarget);
    L.setField(-2, "ClearFollowTarget");

    L.push(Camera_LookAt);
    L.setField(-2, "LookAt");

    L.push(Camera_GetH);
    L.setField(-2, "GetH");

    L.push(Camera_SetH);
    L.setField(-2, "SetH");
    
    L.setGlobal("Camera");
    
    // ========================================================================
    // AUDIO API (Placeholder)
    // ========================================================================
    L.newTable();  // Audio table
    
    L.push(Audio_Play);
    L.setField(-2, "Play");
    
    L.push(Audio_Find);
    L.setField(-2, "Find");
    
    L.push(Audio_Stop);
    L.setField(-2, "Stop");
    
    L.push(Audio_SetVolume);
    L.setField(-2, "SetVolume");
    
    L.push(Audio_StopAll);
    L.setField(-2, "StopAll");

    L.push(Audio_PlayCDDA);
    L.setField(-2, "PlayCDDA");

    L.push(Audio_ResumeCDDA);
    L.setField(-2, "ResumeCDDA");

    L.push(Audio_PauseCDDA);
    L.setField(-2, "PauseCDDA");

    L.push(Audio_StopCDDA);
    L.setField(-2, "StopCDDA");

    L.push(Audio_TellCDDA);
    L.setField(-2, "TellCDDA");

    L.push(Audio_SetCDDAVolume);
    L.setField(-2, "SetCDDAVolume");
    
    L.setGlobal("Audio");
    
    // ========================================================================
    // DEBUG API
    // ========================================================================
    L.newTable();  // Debug table
    
    L.push(Debug_Log);
    L.setField(-2, "Log");
    
    L.push(Debug_DrawLine);
    L.setField(-2, "DrawLine");
    
    L.push(Debug_DrawBox);
    L.setField(-2, "DrawBox");
    
    L.setGlobal("Debug");
    
    // ========================================================================
    // CONVERT API
    // ========================================================================
    L.newTable();  // Convert table
    
    L.push(Convert_IntToFp);
    L.setField(-2, "IntToFp");

    L.push(Convert_FpToInt);
    L.setField(-2, "FpToInt");
    
    L.setGlobal("Convert");

    // ========================================================================
    // MATH API
    // ========================================================================
    L.newTable();  // PSXMath table (avoid conflict with Lua's math)
    
    L.push(Math_Clamp);
    L.setField(-2, "Clamp");
    
    L.push(Math_Lerp);
    L.setField(-2, "Lerp");
    
    L.push(Math_Sign);
    L.setField(-2, "Sign");
    
    L.push(Math_Abs);
    L.setField(-2, "Abs");
    
    L.push(Math_Min);
    L.setField(-2, "Min");
    
    L.push(Math_Max);
    L.setField(-2, "Max");

    L.push(Math_Cos);
    L.setField(-2, "Cos");
    
    L.push(Math_Sin);
    L.setField(-2, "Sin");

    L.push(Math_Convert3DTo2D);
    L.setField(-2, "Convert3DTo2D");

    L.setGlobal("PSXMath");
    
    // ========================================================================
    // RANDOM API
    // ========================================================================

    L.newTable();  // Random table
    
    L.push(Random_Number);
    L.setField(-2, "Number");
    
    L.push(Random_GeneratorNumber);
    L.setField(-2, "GeneratorNumber");

    L.push(Random_Range);
    L.setField(-2, "Range");

    L.push(Random_GeneratorRange);
    L.setField(-2, "GeneratorRange");
    
    L.push(Random_GeneratorSeed);
    L.setField(-2, "GeneratorSeed");
    
    L.setGlobal("Random");

    // ========================================================================
    // SCENE API
    // ========================================================================
    L.newTable();  // Scene table
    
    L.push(Scene_Load);
    L.setField(-2, "Load");
    
    L.push(Scene_GetIndex);
    L.setField(-2, "GetIndex");
    
    L.setGlobal("Scene");
    
    // ========================================================================
    // PERSIST API
    // ========================================================================
    L.newTable();  // Persist table
    
    L.push(Persist_Get);
    L.setField(-2, "Get");
    
    L.push(Persist_Set);
    L.setField(-2, "Set");
    
    L.setGlobal("Persist");

    // ========================================================================
    // MEMCARD API
    // ========================================================================
    L.newTable();  // MemCard table

    L.push(MemCard_IsPresent);
    L.setField(-2, "IsPresent");

    L.push(MemCard_Format);
    L.setField(-2, "Format");

    L.push(MemCard_Save);
    L.setField(-2, "Save");

    L.push(MemCard_Load);
    L.setField(-2, "Load");

    L.push(MemCard_Delete);
    L.setField(-2, "Delete");

    L.push(MemCard_List);
    L.setField(-2, "List");

    L.push(MemCard_FreeBlocks);
    L.setField(-2, "FreeBlocks");

    L.setGlobal("MemCard");

    // ========================================================================
    // CUTSCENE API
    // ========================================================================
    L.newTable();  // Cutscene table
    
    L.push(Cutscene_Play);
    L.setField(-2, "Play");
    
    L.push(Cutscene_Stop);
    L.setField(-2, "Stop");
    
    L.push(Cutscene_IsPlaying);
    L.setField(-2, "IsPlaying");
    
    L.setGlobal("Cutscene");

    // ========================================================================
    // ANIMATION API
    // ========================================================================
    L.newTable();

    L.push(Animation_Play);
    L.setField(-2, "Play");

    L.push(Animation_Stop);
    L.setField(-2, "Stop");

    L.push(Animation_IsPlaying);
    L.setField(-2, "IsPlaying");

    L.setGlobal("Animation");

    // ========================================================================
    // SKINNED ANIMATION API
    // ========================================================================
    L.newTable();

    L.push(SkinnedAnim_Play);
    L.setField(-2, "Play");

    L.push(SkinnedAnim_Stop);
    L.setField(-2, "Stop");

    L.push(SkinnedAnim_IsPlaying);
    L.setField(-2, "IsPlaying");

    L.push(SkinnedAnim_GetClip);
    L.setField(-2, "GetClip");

    L.setGlobal("SkinnedAnim");

    // ========================================================================
    // CONTROLS API
    // ========================================================================
    L.newTable();

    L.push(Controls_SetEnabledPlayer1);
    L.setField(-2, "SetEnabledPlayer1");

    L.push(Controls_IsEnabledPlayer1);
    L.setField(-2, "IsEnabledPlayer1");

    L.push(Controls_SetEnabledPlayer2);
    L.setField(-2, "SetEnabledPlayer2");

    L.push(Controls_IsEnabledPlayer2);
    L.setField(-2, "IsEnabledPlayer2");

    L.setGlobal("Controls");

    // ========================================================================
    // INTERACT API
    // ========================================================================
    L.newTable();

    L.push(Interact_SetEnabled);
    L.setField(-2, "SetEnabled");

    L.push(Interact_IsEnabled);
    L.setField(-2, "IsEnabled");

    L.setGlobal("Interact");

    // ========================================================================
    // UI API
    // ========================================================================
    L.newTable();  // UI table
    
    L.push(UI_FindCanvas);
    L.setField(-2, "FindCanvas");
    
    L.push(UI_SetCanvasVisible);
    L.setField(-2, "SetCanvasVisible");
    
    L.push(UI_IsCanvasVisible);
    L.setField(-2, "IsCanvasVisible");
    
    L.push(UI_FindElement);
    L.setField(-2, "FindElement");
    
    L.push(UI_SetVisible);
    L.setField(-2, "SetVisible");
    
    L.push(UI_IsVisible);
    L.setField(-2, "IsVisible");
    
    L.push(UI_SetText);
    L.setField(-2, "SetText");

    L.push(UI_GetText);
    L.setField(-2, "GetText");
    
    L.push(UI_SetProgress);
    L.setField(-2, "SetProgress");
    
    L.push(UI_GetProgress);
    L.setField(-2, "GetProgress");
    
    L.push(UI_SetColor);
    L.setField(-2, "SetColor");

    L.push(UI_GetColor);
    L.setField(-2, "GetColor");
    
    L.push(UI_SetPosition);
    L.setField(-2, "SetPosition");

    L.push(UI_GetPosition);
    L.setField(-2, "GetPosition");

    L.push(UI_SetSize);
    L.setField(-2, "SetSize");

    L.push(UI_GetSize);
    L.setField(-2, "GetSize");

    L.push(UI_SetProgressColors);
    L.setField(-2, "SetProgressColors");

    L.push(UI_GetElementType);
    L.setField(-2, "GetElementType");

    L.push(UI_GetElementCount);
    L.setField(-2, "GetElementCount");

    L.push(UI_GetElementByIndex);
    L.setField(-2, "GetElementByIndex");
    
    L.push(UI_DrawLine);
    L.setField(-2, "DrawLine");

    L.push(UI_DrawTriangle);
    L.setField(-2, "DrawTriangle");

    L.setGlobal("UI");

    // ========================================================================
    // SPRITE API
    // ========================================================================
    L.newTable();  // Sprite table

    L.push(Sprite_SheetIndex);
    L.setField(-2, "SheetIndex");
    L.push(Sprite_AnimIndex);
    L.setField(-2, "AnimIndex");
    L.push(Sprite_Create);
    L.setField(-2, "Create");
    L.push(Sprite_Destroy);
    L.setField(-2, "Destroy");
    L.push(Sprite_SetPos);
    L.setField(-2, "SetPos");
    L.push(Sprite_SetWorldPos);
    L.setField(-2, "SetWorldPos");
    L.push(Sprite_BindToActor);
    L.setField(-2, "BindToActor");
    L.push(Sprite_SetFrame);
    L.setField(-2, "SetFrame");
    L.push(Sprite_PlayAnim);
    L.setField(-2, "PlayAnim");
    L.push(Sprite_StopAnim);
    L.setField(-2, "StopAnim");
    L.push(Sprite_SetFacingFromYaw);
    L.setField(-2, "SetFacingFromYaw");
    L.push(Sprite_SetVisible);
    L.setField(-2, "SetVisible");
    L.push(Sprite_IsVisible);
    L.setField(-2, "IsVisible");
    L.push(Sprite_SetFlip);
    L.setField(-2, "SetFlip");
    L.push(Sprite_SetColor);
    L.setField(-2, "SetColor");
    L.push(Sprite_SetLayer);
    L.setField(-2, "SetLayer");
    L.push(Sprite_SetSize);
    L.setField(-2, "SetSize");
    L.push(Sprite_SetIgnoreViewOffset);
    L.setField(-2, "SetIgnoreViewOffset");
    L.push(Sprite_SetViewOffset);
    L.setField(-2, "SetViewOffset");
    L.push(Sprite_GetViewOffset);
    L.setField(-2, "GetViewOffset");
    L.push(Sprite_Count);
    L.setField(-2, "Count");

    L.setGlobal("Sprite");

    // ========================================================================
    // TILE API
    // ========================================================================
    L.newTable();  // Tile table

    L.push(Tile_Walkable);
    L.setField(-2, "Walkable");
    L.push(Tile_RayClear);
    L.setField(-2, "RayClear");
    L.push(Tile_MoveActor);
    L.setField(-2, "MoveActor");
    L.push(Tile_Active);
    L.setField(-2, "Active");
    L.push(Tile_MapSize);
    L.setField(-2, "MapSize");
    L.push(Tile_ObjectCount);
    L.setField(-2, "ObjectCount");
    L.push(Tile_ObjectAt);
    L.setField(-2, "ObjectAt");

    // No Tile.KIND here on purpose: the engine attaches no meaning to an object's
    // kind byte. A game defines its own kind constants, and the values only have
    // to agree with what its tilemap was painted with.
    L.setGlobal("Tile");

    // ========================================================================
    // PLAYER API
    // ========================================================================

    L.newTable();  
    
    L.push(Player_SetPosition);
    L.setField(-2, "SetPosition");

    L.push(Player_GetPosition);
    L.setField(-2, "GetPosition");

    L.push(Player_SetRotation);
    L.setField(-2, "SetRotation");

    L.push(Player_GetRotation);
    L.setField(-2, "GetRotation");

    L.setGlobal("Player");
}

// ============================================================================
// ENTITY API IMPLEMENTATION
// ============================================================================

int LuaAPI::Entity_FindByScriptIndex(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push();
        return 1;
    }
    
    // Find first object with matching luaFileIndex
    int16_t luaIdx = static_cast<int16_t>(lua.toNumber(1));
    for (size_t i = 0; i < s_sceneManager->getGameObjectCount(); i++) {
        auto* go = s_sceneManager->getGameObject(static_cast<uint16_t>(i));
        if (go && go->luaFileIndex == luaIdx) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) return 1;
            lua.pop();
        }
    }
    
    lua.push();
    return 1;
}

int LuaAPI::Entity_FindByIndex(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isNumber(1)) {
        lua.push();
        return 1;
    }
    
    int index = static_cast<int>(lua.toNumber(1));
    
    if (s_sceneManager) {
        GameObject* go = s_sceneManager->getGameObject(static_cast<uint16_t>(index));
        if (go) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) {
                return 1;
            }
            lua.pop();
        }
    }
    
    lua.push();
    return 1;
}

int LuaAPI::Entity_Find(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    // Accept number (index) or string (name lookup) for backwards compat
    // Check isNumber FIRST — in Lua, numbers pass isString too.
    if (lua.isNumber(1)) {
        int index = static_cast<int>(lua.toNumber(1));
        GameObject* go = s_sceneManager->getGameObject(static_cast<uint16_t>(index));
        if (go) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) return 1;
            lua.pop();
        }
    } else if (lua.isString(1)) {
        const char* name = lua.toString(1);
        GameObject* go = s_sceneManager->findObjectByName(name);
        if (go) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) return 1;
            lua.pop();
        }
    }

    lua.push();
    return 1;
}

int LuaAPI::Entity_GetCount(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_sceneManager) {
        lua.pushNumber(static_cast<lua_Number>(s_sceneManager->getGameObjectCount()));
    } else {
        lua.pushNumber(0);
    }
    return 1;
}


int LuaAPI::Entity_SetActive(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        return 0;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    bool active = lua.toBoolean(2);
    
    if (go && s_sceneManager) {
        s_sceneManager->setObjectActive(go, active);
    }
    
    return 0;
}

int LuaAPI::Entity_IsActive(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push(false);
        return 1;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (go) {
        lua.push(go->isActive());
    } else {
        lua.push(false);
    }
    
    return 1;
}

int LuaAPI::Entity_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push();
        return 1;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (go) {
        PushVec3(lua, go->position.x, go->position.y, go->position.z);
        return 1;
    }
    
    lua.push();
    return 1;
}

int LuaAPI::Entity_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        return 0;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (!go) return 0;
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 2, x, y, z);

    LuaUtility::SetPosition(go,x,y,z);
    
    return 0;
}

int LuaAPI::Entity_GetRotationY(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.pushNumber(0);
        return 1;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (!go) { lua.pushNumber(0); return 1; }
    
    // Y rotation matrix: vs[0].x = cos(θ), vs[0].z = sin(θ)
    int32_t sinRaw = go->rotation.vs[0].z.raw();
    int32_t cosRaw = go->rotation.vs[0].x.raw();
    
    lua.push(LuaUtility::ToFp12(psyqo::atan2(sinRaw, cosRaw)));
    return 1;
}

int LuaAPI::Entity_SetRotationY(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    if (!go) return 0;

    // Accept FixedPoint or number, convert to Angle (FixedPoint<10>)
    psyqo::FixedPoint<12> fp12 = readFP(lua, 2);
    psyqo::Angle angle;
    angle.value = fp12.value >> 2;
    go->rotation = psxsplash::transposeMatrix33(
        psyqo::SoftMath::generateRotationMatrix33(angle, psyqo::SoftMath::Axis::Y, s_trig));
    return 0;
}

int LuaAPI::Entity_SetRotation(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    if (!go) return 0;


    // Accept three angles in pi-units (e.g., 0.5 = π/2 = 90°)
    // This matches psyqo::Angle convention used by the engine.
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 2, x, y, z);

    // Convert to Angle (FixedPoint<10>) 
    psyqo::Angle rx, ry, rz;
    rx.value = x.value >> 2;
    ry.value = y.value >> 2;
    rz.value = z.value >> 2;

    auto matY = psyqo::SoftMath::generateRotationMatrix33(ry, psyqo::SoftMath::Axis::Y, s_trig);
    auto matX = psyqo::SoftMath::generateRotationMatrix33(rx, psyqo::SoftMath::Axis::X, s_trig);
    auto matZ = psyqo::SoftMath::generateRotationMatrix33(rz, psyqo::SoftMath::Axis::Z, s_trig);
    psyqo::Matrix33 temp;
    psyqo::GteMath::multiplyMatrix33(matY, matX, &temp);
    // Aliasing out onto an input is explicitly supported.
    psyqo::GteMath::multiplyMatrix33(temp, matZ, &temp);
    go->rotation = psxsplash::transposeMatrix33(temp);

    return 0;
}


int LuaAPI::Entity_GetForward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    if (!go) {
        psyqo::FixedPoint<12> zero(0);
        PushVec3(lua, zero, zero, zero);
        return 1;
    }

    psyqo::Vec3 directionVector = LuaUtility::GetForward(go->rotation);

    PushVec3(lua, directionVector.x, directionVector.y, directionVector.z);
    return 1;
}

int LuaAPI::Entity_GetRight(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    if (!go) {
        psyqo::FixedPoint<12> zero(0);
        PushVec3(lua, zero, zero, zero);
        return 1;
    }

    psyqo::Vec3 directionVector = LuaUtility::GetRight(go->rotation);

    PushVec3(lua, directionVector.x, directionVector.y, directionVector.z);
    return 1;
}

int LuaAPI::Entity_GetUp(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    if (!go) {
        psyqo::FixedPoint<12> zero(0);
        PushVec3(lua, zero, zero, zero);
        return 1;
    }

    psyqo::Vec3 directionVector = LuaUtility::GetUp(go->rotation);

    PushVec3(lua, directionVector.x, directionVector.y, directionVector.z);
    return 1;
}

int LuaAPI::Entity_MoveForward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");

    auto go = lua.toUserdata<GameObject>(-1);

    lua.pop();

    if (!go) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 2);

    psyqo::Vec3 newPos = LuaUtility::GetForward(go->rotation) * stepAmount;
    newPos = newPos + go->position;

    LuaUtility::SetPosition(go,newPos);
   
    return 0;
}

int LuaAPI::Entity_MoveBackward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");

    auto go = lua.toUserdata<GameObject>(-1);

    lua.pop();

    if (!go) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 2);

    psyqo::Vec3 newPos = LuaUtility::GetBackward(go->rotation) * stepAmount;
    newPos = newPos + go->position;

    LuaUtility::SetPosition(go,newPos);
   
    return 0;
}

int LuaAPI::Entity_MoveLeft(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");

    auto go = lua.toUserdata<GameObject>(-1);

    lua.pop();

    if (!go) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 2);

    psyqo::Vec3 newPos = LuaUtility::GetLeft(go->rotation) * stepAmount;
    newPos = newPos + go->position;

    LuaUtility::SetPosition(go,newPos);
   
    return 0;
}

int LuaAPI::Entity_MoveRight(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");

    auto go = lua.toUserdata<GameObject>(-1);

    lua.pop();

    if (!go) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 2);

    psyqo::Vec3 newPos = LuaUtility::GetRight(go->rotation) * stepAmount;
    newPos = newPos + go->position;

    LuaUtility::SetPosition(go,newPos);
   
    return 0;
}

int LuaAPI::Entity_MoveUp(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");

    auto go = lua.toUserdata<GameObject>(-1);

    lua.pop();

    if (!go) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 2);

    psyqo::Vec3 newPos = LuaUtility::GetUp(go->rotation) * stepAmount;
    newPos = newPos + go->position;

    LuaUtility::SetPosition(go,newPos);
   
    return 0;
}

int LuaAPI::Entity_MoveDown(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");

    auto go = lua.toUserdata<GameObject>(-1);

    lua.pop();

    if (!go) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 2);

    psyqo::Vec3 newPos = LuaUtility::GetDown(go->rotation) * stepAmount;
    newPos = newPos + go->position;

    LuaUtility::SetPosition(go,newPos);
   
    return 0;
}

int LuaAPI::Entity_ForEach(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isFunction(1)) return 0;
    
    size_t count = s_sceneManager->getGameObjectCount();
    for (size_t i = 0; i < count; i++) {
        auto* go = s_sceneManager->getGameObject(static_cast<uint16_t>(i));
        if (!go || !go->isActive()) continue;
        
        // Push callback copy
        lua.copy(1);
        // Look up registered Lua table for this object (keyed by C++ pointer)
        lua.push(reinterpret_cast<uint8_t*>(go));
        lua.rawGet(LUA_REGISTRYINDEX);
        if (!lua.isTable(-1)) {
            lua.pop(2);  // pop non-table + callback copy
            continue;
        }
        lua.pushNumber(i);  // push index as second argument
        if (lua.pcall(2, 0) != LUA_OK) {
            lua.pop();  // pop error message
        }
    }
    
    return 0;
}

int LuaAPI::Entity_SetUVOffset(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    if (!go) return 0;

    // Absolute UV offset applied non-destructively at render time (also the
    // field animated by TrackType::ObjectUVOffset). Unlike SetUVs, this leaves
    // the source polygon UVs untouched.
    int u = (int)lua.checkNumber(2);
    int v = (int)lua.checkNumber(3);

    go->uvOffset.u = (uint8_t)u;
    go->uvOffset.v = (uint8_t)v;

    return 0;
}

int LuaAPI::Entity_SetUVs(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1) || !lua.isTable(2))
    {
        return 0;
    }

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    // get index 1 from table 2
    lua.rawGetI(2, 1);
    int xVal = (int)lua.checkNumber(-1);
    lua.pop();

    // get index 2 from table 2
    lua.rawGetI(2, 2);
    int yVal = (int)lua.checkNumber(-1);
    lua.pop();

    if (!go) return 0;

    // check if the gameobject has polygons
    if (go->polyCount > 0)
    {
        // alter the uvs of the polygons
        for (int i = 0; i < go->polyCount; i++)
        {
            go->polygons[i].uvA.u = (go->polygons[i].uvA.u + xVal);
            go->polygons[i].uvA.v = (go->polygons[i].uvA.v + yVal);

            go->polygons[i].uvB.u = (go->polygons[i].uvB.u + xVal);
            go->polygons[i].uvB.v = (go->polygons[i].uvB.v + yVal);

            go->polygons[i].uvC.u = (go->polygons[i].uvC.u + xVal);
            go->polygons[i].uvC.v = (go->polygons[i].uvC.v + yVal);
        }
    }

    return 0;
}

int LuaAPI::Entity_SetTPage(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1) || !lua.isTable(2))
    {
        return 0;
    }

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    // get index 1 from table 2
    lua.rawGetI(2, 1);
    int xVal = (int)lua.checkNumber(-1);
    lua.pop();

    // get index 2 from table 2
    lua.rawGetI(2, 2);
    int yVal = (int)lua.checkNumber(-1);
    lua.pop();

    if (!go) return 0;

    // check if the gameobject has polygons
    if (go->polyCount > 0)
    {
        // change the texture page of the polygons
        for (int i = 0; i < go->polyCount; i++)
        {
            go->polygons[i].tpage.setPageX(xVal);
            go->polygons[i].tpage.setPageY(yVal);
        }
    }

    return 0;
}

int LuaAPI::Entity_SetParent(lua_State* L)
{
    psyqo::Lua lua(L);

    if (!lua.isTable(1) || !lua.isTable(2))
        return 0;

    lua.getField(1, "__cpp_ptr");
    auto goParent = lua.toUserdata<GameObject>(-1);
    lua.pop();

    lua.getField(2, "__cpp_ptr");
    auto goChild = lua.toUserdata<GameObject>(-1);
    lua.pop();

    psyqo::FixedPoint<12> offX, offY, offZ;
    ReadVec3(lua, 3, offX, offY, offZ);

    if (!goParent || !goChild)
        return 0;

    //offX = psyqo::FixedPoint<12>(0.025_fp);
    //offY = psyqo::FixedPoint<12>(0.025_fp);
    //offZ = psyqo::FixedPoint<12>(0.025_fp);

    psyqo::Vec3 localOffset = {
        .x = offX,
        .y = offY,
        .z = offZ,
    };

    psyqo::Vec3 worldOffset;

    worldOffset.x =
        psyqo::FixedPoint<12>(
            (goParent->rotation.vs[0].x * localOffset.x +
                goParent->rotation.vs[0].y * localOffset.y +
                goParent->rotation.vs[0].z * localOffset.z)
        );

    worldOffset.y =
        psyqo::FixedPoint<12>(
            (goParent->rotation.vs[1].x * localOffset.x +
                goParent->rotation.vs[1].y * localOffset.y +
                goParent->rotation.vs[1].z * localOffset.z)
        );

    worldOffset.z =
        psyqo::FixedPoint<12>(
            (goParent->rotation.vs[2].x * localOffset.x +
                goParent->rotation.vs[2].y * localOffset.y +
                goParent->rotation.vs[2].z * localOffset.z)
        );

    goChild->position.x = goParent->position.x + worldOffset.x;
    goChild->position.y = goParent->position.y + worldOffset.y;
    goChild->position.z = goParent->position.z + worldOffset.z;

    goChild->rotation = goParent->rotation;

    return 0;
}

// ============================================================================
// VEC3 API IMPLEMENTATION
// ============================================================================

void LuaAPI::PushVec3(psyqo::Lua& L, psyqo::FixedPoint<12> x,
                      psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z) {
    L.newTable();
    L.push(x);
    L.setField(-2, "x");
    L.push(y);
    L.setField(-2, "y");
    L.push(z);
    L.setField(-2, "z");
}

void LuaAPI::ReadVec3(psyqo::Lua& L, int idx,
                      psyqo::FixedPoint<12>& x,
                      psyqo::FixedPoint<12>& y,
                      psyqo::FixedPoint<12>& z) {
    L.getField(idx, "x");
    x = readFP(L, -1);
    L.pop();

    L.getField(idx, "y");
    y = readFP(L, -1);
    L.pop();

    L.getField(idx, "z");
    z = readFP(L, -1);
    L.pop();
}

int LuaAPI::Vec3_New(lua_State* L) {
    psyqo::Lua lua(L);

    psyqo::FixedPoint<12> x = lua.isNoneOrNil(1) ? psyqo::FixedPoint<12>() : readFP(lua, 1);
    psyqo::FixedPoint<12> y = lua.isNoneOrNil(2) ? psyqo::FixedPoint<12>() : readFP(lua, 2);
    psyqo::FixedPoint<12> z = lua.isNoneOrNil(3) ? psyqo::FixedPoint<12>() : readFP(lua, 3);

    PushVec3(lua, x, y, z);
    return 1;
}

int LuaAPI::Vec3_Add(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    PushVec3(lua, ax + bx, ay + by, az + bz);
    return 1;
}

int LuaAPI::Vec3_Sub(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    PushVec3(lua, ax - bx, ay - by, az - bz);
    return 1;
}

int LuaAPI::Vec3_Mul(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    psyqo::FixedPoint<12> scalar = readFP(lua, 2);
    
    PushVec3(lua, x * scalar, y * scalar, z * scalar);
    return 1;
}

int LuaAPI::Vec3_Dot(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    auto dot = ax * bx + ay * by + az * bz;
    lua.push(dot);
    return 1;
}

int LuaAPI::Vec3_Cross(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    psyqo::FixedPoint<12> cx = ay * bz - az * by;
    psyqo::FixedPoint<12> cy = az * bx - ax * bz;
    psyqo::FixedPoint<12> cz = ax * by - ay * bx;
    
    PushVec3(lua, cx, cy, cz);
    return 1;
}

int LuaAPI::Vec3_LengthSq(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    auto lengthSq = x * x + y * y + z * z;
    lua.push(lengthSq);
    return 1;
}

int LuaAPI::Vec3_Length(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    // lengthSq in fp12: (x*x + y*y + z*z) is fp24 (two fp12 multiplied).
    // We need sqrt(lengthSq) as fp12.
    // lengthSq raw = sum of (raw*raw >> 12) values = fp12 result
    auto lengthSq = x * x + y * y + z * z;
    int32_t lsRaw = lengthSq.raw();

    if (lsRaw <= 0) {
        lua.push(psyqo::FixedPoint<12>());
        return 1;
    }

    // Integer sqrt of (lsRaw << 12) to get result in fp12
    // sqrt(fp12_value) = sqrt(raw/4096) = sqrt(raw)/64
    // So: result_raw = isqrt(raw * 4096) = isqrt(raw << 12)
    // isqrt(lsRaw) gives integer sqrt. Multiply by 64 (sqrt(4096)) to get fp12.
    // Newton's method in 32-bit: isqrt(n)
    uint32_t n = (uint32_t)lsRaw;
    uint32_t guess = n;
    for (int i = 0; i < 16; i++) {
        if (guess == 0) break;
        guess = (guess + n / guess) / 2;
    }
    // guess = isqrt(lsRaw). lsRaw is in fp12, so sqrt needs * sqrt(4096) = 64
    psyqo::FixedPoint<12> result;
    result.value = (int32_t)(guess * 64);
    lua.push(result);
    return 1;
}

int LuaAPI::Vec3_Normalize(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    auto lengthSq = x * x + y * y + z * z;
    int32_t lsRaw = lengthSq.raw();

    if (lsRaw <= 0) {
        PushVec3(lua, psyqo::FixedPoint<12>(), psyqo::FixedPoint<12>(), psyqo::FixedPoint<12>());
        return 1;
    }

    // isqrt(lsRaw) * 64 = length in fp12
    uint32_t n = (uint32_t)lsRaw;
    uint32_t guess = n;
    for (int i = 0; i < 16; i++) {
        if (guess == 0) break;
        guess = (guess + n / guess) / 2;
    }
    int32_t len = (int32_t)(guess * 64);
    if (len == 0) len = 1;

    // Divide each component by length: component / length in fp12
    // (x.raw * 4096) / len using 32-bit math (safe since raw values fit int16 range)
    psyqo::FixedPoint<12> nx, ny, nz;
    nx.value = (x.raw() * 4096) / len;
    ny.value = (y.raw() * 4096) / len;
    nz.value = (z.raw() * 4096) / len;
    PushVec3(lua, nx, ny, nz);
    return 1;
}

int LuaAPI::Vec3_DistanceSq(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    auto dx = ax - bx;
    auto dy = ay - by;
    auto dz = az - bz;
    
    auto distSq = dx * dx + dy * dy + dz * dz;
    lua.push(distSq);
    return 1;
}

int LuaAPI::Vec3_Distance(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    auto dx = ax - bx;
    auto dy = ay - by;
    auto dz = az - bz;

    auto distSq = dx * dx + dy * dy + dz * dz;
    int32_t dsRaw = distSq.raw();

    if (dsRaw <= 0) {
        lua.push(psyqo::FixedPoint<12>());
        return 1;
    }

    uint32_t n = (uint32_t)dsRaw;
    uint32_t guess = n;
    for (int i = 0; i < 16; i++) {
        if (guess == 0) break;
        guess = (guess + n / guess) / 2;
    }

    psyqo::FixedPoint<12> result;
    result.value = (int32_t)(guess * 64);
    lua.push(result);
    return 1;
}

int LuaAPI::Vec3_Lerp(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    psyqo::FixedPoint<12> t = readFP(lua, 3);
    psyqo::FixedPoint<12> oneMinusT = psyqo::FixedPoint<12>(4096, psyqo::FixedPoint<12>::RAW) - t;
    
    psyqo::FixedPoint<12> rx = ax * oneMinusT + bx * t;
    psyqo::FixedPoint<12> ry = ay * oneMinusT + by * t;
    psyqo::FixedPoint<12> rz = az * oneMinusT + bz * t;
    
    PushVec3(lua, rx, ry, rz);
    return 1;
}

// ============================================================================
// INPUT API IMPLEMENTATION
// ============================================================================

void LuaAPI::RegisterInputConstants(psyqo::Lua& L) {
    // Button constants - must match psyqo::AdvancedPad::Button enum
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Cross));
    L.setField(-2, "CROSS");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Circle));
    L.setField(-2, "CIRCLE");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Square));
    L.setField(-2, "SQUARE");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Triangle));
    L.setField(-2, "TRIANGLE");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::L1));
    L.setField(-2, "L1");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::R1));
    L.setField(-2, "R1");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::L2));
    L.setField(-2, "L2");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::R2));
    L.setField(-2, "R2");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Start));
    L.setField(-2, "START");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Select));
    L.setField(-2, "SELECT");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Up));
    L.setField(-2, "UP");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Down));
    L.setField(-2, "DOWN");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Left));
    L.setField(-2, "LEFT");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Right));
    L.setField(-2, "RIGHT");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::L3));
    L.setField(-2, "L3");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::R3));
    L.setField(-2, "R3");
}

int LuaAPI::Input_IsPressedPlayer1(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    
    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControlsPlayer1().wasButtonPressed(button));
    return 1;
}

int LuaAPI::Input_IsPressedPlayer2(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }

    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControlsPlayer2().wasButtonPressed(button));
    return 1;
}

int LuaAPI::Input_IsReleasedPlayer1(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    
    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControlsPlayer1().wasButtonReleased(button));
    return 1;
}

int LuaAPI::Input_IsReleasedPlayer2(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }

    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControlsPlayer2().wasButtonReleased(button));
    return 1;
}

int LuaAPI::Input_IsHeldPlayer1(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    
    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControlsPlayer1().isButtonHeld(button));
    return 1;
}

int LuaAPI::Input_IsHeldPlayer2(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }

    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControlsPlayer2().isButtonHeld(button));
    return 1;
}

int LuaAPI::Input_GetAnalogPlayer1(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager) {
        lua.pushNumber(0);
        lua.pushNumber(0);
        return 2;
    }
    
    int stick = lua.isNumber(1) ? static_cast<int>(lua.toNumber(1)) : 0;
    auto& controls = s_sceneManager->getControlsPlayer1();
    
    int16_t x, y;
    if (stick == 1) {
        x = controls.getRightStickX();
        y = controls.getRightStickY();
    } else {
        x = controls.getLeftStickX();
        y = controls.getLeftStickY();
    }
    
    // Scale to approximately [-1.0, 1.0] in Lua number space
    // Stick range is -127 to +127; divide by 127
    lua.pushNumber(x * kFixedScale / 127);
    lua.pushNumber(y * kFixedScale / 127);
    return 2;
}

int LuaAPI::Input_GetAnalogPlayer2(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager) {
        lua.pushNumber(0);
        lua.pushNumber(0);
        return 2;
    }

    int stick = lua.isNumber(1) ? static_cast<int>(lua.toNumber(1)) : 0;
    auto& controls = s_sceneManager->getControlsPlayer2();

    int16_t x, y;
    if (stick == 1) {
        x = controls.getRightStickX();
        y = controls.getRightStickY();
    }
    else {
        x = controls.getLeftStickX();
        y = controls.getLeftStickY();
    }

    // Scale to approximately [-1.0, 1.0] in Lua number space
    // Stick range is -127 to +127; divide by 127
    lua.pushNumber(x * kFixedScale / 127);
    lua.pushNumber(y * kFixedScale / 127);
    return 2;
}

int LuaAPI::Input_BindToActor(lua_State* L) {
    psyqo::Lua lua(L);
    // Input.BindToActor(player, actor) - player is 1 or 2. Bind to the player
    // actor (Actor.GetPlayer()) to restore default player locomotion.
    int player = static_cast<int>(lua.checkNumber(1)) - 1;
    uint16_t actorId = ReadActorId(lua, 2);
    if (s_sceneManager) s_sceneManager->setControlBoundActor(player, actorId);
    return 0;
}

int LuaAPI::Input_GetBoundActor(lua_State* L) {
    psyqo::Lua lua(L);
    int player = static_cast<int>(lua.checkNumber(1)) - 1;
    uint16_t actorId = s_sceneManager ? s_sceneManager->getControlBoundActor(player) : 0;
    lua.pushNumber(static_cast<lua_Number>(actorId));
    return 1;
}

// ============================================================================
// TIMER API IMPLEMENTATION
// ============================================================================

static uint32_t s_frameCount = 0;

void LuaAPI::IncrementFrameCount() {
    s_frameCount++;
    // Advance the shared RNG once per frame so draws depend on timing. It is not
    // multiplied into the seed: repeated even factors shift in zero bits until the
    // xorshift state is 0, which it never leaves. m_randomGenerator is
    // deliberately untouched: it is the explicitly-seeded, reproducible generator
    // that Random.GeneratorSeed owns.
    SceneManager::m_random.rand();
}

// ============================================================================
// NET API IMPLEMENTATION (serial multiplayer over SIO1)
// ============================================================================

int LuaAPI::Net_Connect(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push(false);
        return 1;
    }
    // Scene identity: both consoles running the same scene agree on this, so a
    // mismatch (different scene/build) is detected during the handshake.
    //
    // Prefer the authored id (splashpack v22+). The derived fallback below is
    // wrong in two ways that only bite once a scene is edited: it collides
    // between scenes that happen to share an actor count, and it CHANGES when
    // anyone adds an object - silently splitting a room across two builds.
    uint32_t sceneHash = s_sceneManager->getAuthoredSceneHash();
    if (sceneHash == 0) {
        uint32_t sceneIndex = static_cast<uint32_t>(s_sceneManager->getCurrentSceneIndex());
        uint32_t actorCount = static_cast<uint32_t>(s_sceneManager->getActorCount());
        sceneHash = (sceneIndex * 2654435761u) ^ (actorCount * 40503u) ^ 0x5A5A0000u;
    }
    // Host-election entropy: a hardware timer sample that differs per console.
    uint32_t seed = s_sceneManager->getFrameTimestamp() ^ (s_frameCount << 3) ^ SceneManager::m_random.rand();

    // Net.Connect([baud[, rxMode]]).
    //
    // rxMode: 0/nil = Auto (polled under Redux, interrupt on hardware), 1 =
    // Polled, 2 = Interrupt. The override exists because Auto's hardware choice
    // is the one path that cannot be tested without a console: if a real machine
    // sits at "connecting" forever, forcing Polled says whether the RX interrupt
    // is at fault, and forcing Interrupt under an emulator says the opposite.
    // The default MUST come from Sio1, not a literal. This line held its own copy
    // of 115200, so changing Sio1::c_defaultBaud silently did nothing: the console
    // kept transmitting at the old rate while the bridge moved to the new one, and
    // a baud mismatch presents as a completely dead link with no diagnostic. One
    // number, one place.
    const uint32_t baud = lua.isNumber(1) ? static_cast<uint32_t>(lua.toNumber(1)) : Sio1::c_defaultBaud;
    Sio1::RxMode rxMode = Sio1::RxMode::Auto;
    if (lua.isNumber(2)) {
        const int m = static_cast<int>(lua.toNumber(2));
        if (m == 1) rxMode = Sio1::RxMode::Polled;
        else if (m == 2) rxMode = Sio1::RxMode::Interrupt;
    }
    NetworkManager::Get().begin(sceneHash, seed, baud, rxMode);
    lua.push(true);
    return 1;
}

int LuaAPI::Net_Disconnect(lua_State* L) {
    (void)L;
    NetworkManager::Get().end();
    return 0;
}

int LuaAPI::Net_IsConnected(lua_State* L) {
    psyqo::Lua lua(L);
    lua.push(NetworkManager::Get().isConnected());
    return 1;
}

int LuaAPI::Net_IsHost(lua_State* L) {
    psyqo::Lua lua(L);
    lua.push(NetworkManager::Get().isHost());
    return 1;
}

int LuaAPI::Net_State(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(static_cast<lua_Number>(static_cast<int>(NetworkManager::Get().state())));
    return 1;
}

/// Net.Stats() -> table of link counters.
///
/// These counters all existed already and NONE of them were reachable. That gap
/// is why a dead serial link is so hard to diagnose: on real hardware
/// `Debug.Log` writes to the BIOS TTY, which on a retail console goes nowhere, so
/// a game that cannot read these numbers has literally no way to report what the
/// link is doing. The only symptom available was "the screen still says
/// connecting" - which turned out to be a frozen frame, not a status.
///
/// Reading the table, when the console will not connect:
///   rxIrqs 0 + bytesRx 0  -> the RX interrupt never fired (event/mask problem)
///   bytesRx rising, frames 0 -> bytes arrive but no frame ever completes
///   frames rising, still connecting -> handshake rejected; check magic/version
///   serialErrors rising   -> the 8-byte hardware FIFO is overrunning
///   crcErrors/resyncs rising -> corruption on the wire; baud or grounding
///   rxHighWater near rxCapacity -> poll() is not keeping up; the frame rate has
///                            fallen far enough that the ring cannot span a frame,
///                            and the largest messages are being shredded first
#include <psyqo/alloc.h>

namespace {
// Heap bytes in use, in KB. Same computation the optional memory overlay does
// (memoverlay.cpp), lifted out from behind its build flag so a RUNNING game can
// report it -- a leak that only shows up after many minutes of play is exactly
// the kind that a debug-only overlay never catches.
extern "C" {
extern char __heap_start;
extern char __stack_start;
}
uint32_t HeapUsedKB() {
    void* heapEnd = psyqo_heap_end();
    if (heapEnd == nullptr) return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(&__heap_start);
    const uintptr_t end = reinterpret_cast<uintptr_t>(heapEnd);
    if (end < base) return 0;
    return static_cast<uint32_t>((end - base) / 1024);
}
}  // namespace

int LuaAPI::Net_Stats(lua_State* L) {
    psyqo::Lua lua(L);
    auto& sio = Sio1::Get();
    const auto& link = NetworkManager::Get().link();

    lua.newTable();
    auto field = [&lua](const char* name, uint32_t v) {
        lua.pushNumber(static_cast<lua_Number>(v));
        lua.setField(-2, name);
    };
    field("bytesRx", sio.bytesReceived());
    field("bytesTx", sio.bytesSent());
    field("rxIrqs", sio.rxInterrupts());
    field("rxOverflows", sio.rxOverflows());
    // Early warning where rxOverflows is a post-mortem: this is how close the ring
    // came to filling, so "one hitch away from loss" is visible before any byte is
    // actually dropped. Compare against rxCapacity.
    field("rxHighWater", sio.rxHighWater());
    field("rxCapacity", sio.rxCapacity());
    // The OUTGOING side. txPending pinned near txCapacity means the console is
    // producing faster than the cable carries - which for a long time it was, by a
    // factor of about a hundred, with nothing on screen to say so.
    field("txPending", sio.txPending());
    field("txHighWater", sio.txHighWater());
    field("txCapacity", sio.txCapacity());
    field("txRejected", sio.txRejected());
    field("snapshotsDropped", NetworkManager::Get().snapshotsDropped());

    // MEASURED, not configured - the distinction that this whole stack lacked.
    //
    // Every number above describes what the driver was TOLD to do or what it
    // counted while doing it. These four describe what the link actually is. Their
    // absence is why two separate week-long hunts ended in guesswork: with a
    // receive path silently capped at 480 B/s, every counter here still read
    // perfectly healthy, and nothing anywhere held a figure that could contradict
    // the assumption.
    //
    // Read them together. goodput far below what the wire should carry means the
    // bottleneck is on this side; rtt far above the wire's own latency means it is
    // queueing somewhere; peerGoodput disagreeing with goodput means the two ends
    // do not agree about the link, which is the most useful signal of the four.
    // rttSamples == 0 means the peer never answered, so rtt and rto are defaults.
    const auto& measured = NetworkManager::Get().link();
    field("goodput", measured.goodputBytesPerSecond());
    field("inbound", measured.inboundBytesPerSecond());
    field("rttMillis", measured.rttMillis());
    field("rtoMillis", measured.rtoMillis());
    field("rttSamples", measured.rttSamples());
    field("peerGoodput", measured.peerGoodput());
    field("peerRttMillis", measured.peerRttMillis());
    // Saturation, split by consequence. latestDeferred means position updates are
    // being held back to keep the queue shallow -- the system working. pingDeferred
    // means the link could not even fit its own measurement probe, which is how a
    // saturated console ends up reporting NO PONG forever.
    field("latestDeferred", measured.latestDeferred());
    field("pingDeferred", measured.pingDeferred());
    // The two numbers that separate "packets are not arriving" from "packets are
    // arriving and playback is wrong" -- the only two explanations for choppy
    // remote movement, and guessing between them costs a disc to test.
    // HEAP USED, in KB. Not a network number, and deliberately on the same line.
    //
    // "It freezes after many minutes" is the signature of something accumulating,
    // and on a 2MB console with no virtual memory the first candidate is the heap.
    // The engine already computed this for its optional overlay (memoverlay.cpp)
    // but behind a build flag and never where a running game could show it, so a
    // slow leak has never once been observable in play. If this number climbs and
    // the console dies near the ceiling, that is the answer; if it is flat, the
    // whole family of leak theories is dead and worth eliminating cheaply.
    field("heapKB", HeapUsedKB());
    // PAD INITS, and it belongs beside heapKB for exactly the same reason: it is
    // the other thing that used to accumulate until the console died.
    //
    // MUST READ 2 AND STAY THERE for the whole boot -- one per player, registered
    // once. It is not a trend line, it is an assertion with a known answer, which
    // is what makes it readable at a glance on a screen that is the only channel
    // this machine has.
    //
    // It used to read 2 x (scene loads). psyqo::AdvancedPad::initialize() appends a
    // readPad() callback to the kernel's per-frame list, nothing can remove one,
    // and SceneManager::InitializeScene called it for both players on every scene
    // load -- so the console permanently gained two more blocking, bit-banged SIO0
    // pad polls per frame on every transition. Combined with the missing timeout in
    // the pad wait and the ~150us blackout of an SIO1 interrupt, that is what froze
    // real consoles: later and later into a session, and never on an emulator.
    // See Controls::Init in controls.hh.
    field("padInits", Controls::padInitCount());
    // RX REVIVALS: times the receive interrupt was found dead and restarted.
    //
    // The single most diagnostic number this console can report. psyqo's pad driver
    // clears the controller interrupt with a read-modify-write on I_STAT, which can
    // acknowledge OUR pending SIO1 interrupt by accident - and since SIO_STAT.9 is
    // sticky while I_STAT.8 is edge-triggered, that kill is permanent unless
    // something re-creates the edge. Sio1::rearmRx() does, and counts it here.
    //
    // 0 means the race never fired this session. Climbing, with the game still
    // playing, means it fired and was repaired in flight - which is the direct
    // confirmation that this was the fault all along.
    field("rxRevivals", sio.rxRevivals());
    field("snapsPerSec", NetworkManager::Get().snapshotsPerSecond());
    field("lerpMs", NetworkManager::Get().lerpIntervalMillis());
    // So a console/bridge baud mismatch can be SEEN. A mismatch is otherwise
    // silent - it presents as framing errors, i.e. as a dead link - and there is
    // no way to read the console's setting off the outside of the machine.
    field("baud", sio.baud());
    // Non-zero means the driver caught its own interrupt handler running away and
    // demoted RX to polling to keep the console alive. The link is degraded from
    // that moment on, so this must be visible rather than inferred from symptoms.
    field("irqStorms", sio.irqStorms());
    field("txSpinTimeouts", sio.txSpinTimeouts());
    // Split, because they are three unrelated faults that were reported as one
    // number called "OVERRUN" -- a diagnosis rather than a measurement, and it
    // cost several hardware runs chasing latency that may not have been the issue.
    //   OE = we were too slow to drain the FIFO (latency)
    //   FE = bit timing disagreement or a marginal signal (cable/baud/grounding)
    field("serialErrors", sio.serialErrors());
    field("rxOverrunErrors", sio.rxOverrunErrors());
    field("rxFramingErrors", sio.rxFramingErrors());
    field("rxParityErrors", sio.rxParityErrors());
    field("rxDrainOverruns", sio.rxDrainOverruns());
    field("acksDeferred", link.acksDeferred());
    field("frames", link.framesReceived());
    field("crcErrors", link.crcErrors());
    field("resyncs", link.resyncs());
    field("queueDepth", NetworkManager::Get().reliableQueueDepth());
    // Which RX path actually resolved, so "works in Redux, dead on console" is
    // visible rather than inferred.
    field("rxMode", static_cast<uint32_t>(sio.rxMode()));
    return 1;
}

int LuaAPI::Net_LocalSlot(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(static_cast<lua_Number>(NetworkManager::Get().localSlot()));
    return 1;
}

int LuaAPI::Net_PlayerCount(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(static_cast<lua_Number>(NetworkManager::Get().playerCount()));
    return 1;
}

int LuaAPI::Net_SetLocalAvatar(lua_State* L) {
    psyqo::Lua lua(L);
    uint16_t actorId = ReadActorId(lua, 1);
    NetworkManager::Get().setLocalAvatarActor(actorId);
    return 0;
}

/// Net.SetReplicationEnabled(bool) -- whether this scene replicates its avatar.
///
/// A menu, a lobby or a cutscene has no avatar worth sending, and sending anyway
/// is not free. A console in an avatar-less scene was measured spending 894 B/s,
/// essentially its entire outbound budget at that frame rate -- broadcasting a
/// player that did not exist, while the reliable message it was waiting for queued
/// behind that traffic.
///
/// Defaults to true, so an existing game that never calls this is unaffected.
int LuaAPI::Net_SetReplicationEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    NetworkManager::Get().setReplicationEnabled(lua.toBoolean(1));
    return 0;
}

int LuaAPI::Net_SetRemoteAvatar(lua_State* L) {
    psyqo::Lua lua(L);
    uint8_t slot = static_cast<uint8_t>(lua.checkNumber(1));
    uint16_t actorId = ReadActorId(lua, 2);
    NetworkManager::Get().setRemoteAvatarActor(slot, actorId);
    return 0;
}

int LuaAPI::Net_RegisterActor(lua_State* L) {
    psyqo::Lua lua(L);
    uint16_t actorId = ReadActorId(lua, 1);
    lua.push(NetworkManager::Get().registerNetworkedActor(actorId));
    return 1;
}

int LuaAPI::Net_UnregisterActor(lua_State* L) {
    psyqo::Lua lua(L);
    uint16_t actorId = ReadActorId(lua, 1);
    NetworkManager::Get().unregisterNetworkedActor(actorId);
    return 0;
}

int LuaAPI::Net_Send(lua_State* L) {
    psyqo::Lua lua(L);
    // Net.Send(eventId [, arg]) -> bool. Reliable; peers receive onNetEvent(id, arg).
    int32_t eventId = static_cast<int32_t>(lua.checkNumber(1));
    int32_t arg = lua.isNoneOrNil(2) ? 0 : static_cast<int32_t>(lua.checkNumber(2));
    lua.push(NetworkManager::Get().sendGameEvent(eventId, arg));
    return 1;
}

int LuaAPI::Net_SyncActor(lua_State* L) {
    psyqo::Lua lua(L);
    // Net.SyncActor(actor) -> bool. Reliably replicates that (object-backed)
    // actor's `self.sync` table to peers, keyed by actorId. Call after mutating
    // self.sync. The player actor has no object/table and cannot be synced this
    // way (use Net.Send for player-specific state).
    if (!s_sceneManager) {
        lua.push(false);
        return 1;
    }
    uint16_t actorId = ReadActorId(lua, 1);
    GameObject* go = s_sceneManager->getActorGameObject(actorId);
    if (!go) {
        lua.push(false);
        return 1;
    }
    uint8_t buf[256];  // [actorId:2][serialized self.sync up to 254]
    buf[0] = static_cast<uint8_t>(actorId & 0xFF);
    buf[1] = static_cast<uint8_t>(actorId >> 8);
    uint32_t blobSize = s_sceneManager->getLua().SerializeObjectSync(go, buf + 2, sizeof(buf) - 2);
    if (blobSize == 0) {
        lua.push(false);  // no self.sync table, or it exceeds the size budget
        return 1;
    }
    bool ok = NetworkManager::Get().sendObjectState(buf, static_cast<uint16_t>(2 + blobSize));
    lua.push(ok);
    return 1;
}

int LuaAPI::Net_SendData(lua_State* L) {
    psyqo::Lua lua(L);
    // Net.SendData(str) -> bool. Sends an opaque payload reliably and in order;
    // the peer's scene script receives it as onNetData(str). The engine never
    // interprets the bytes - this is where a game puts its own protocol (room
    // lists, roles, votes). Unlike Net.Send(id, arg) it can carry strings.
    //
    // Returns false if the payload is too large or the reliable queue is full.
    // CHECK IT: a dropped payload is otherwise silent. Net.ReliableQueueDepth()
    // lets a caller back off before that happens.
    size_t len = 0;
    const char* data = lua.toString(1, &len);
    if (!data || len == 0 || len > net::c_maxEventPayload) {
        lua.push(false);
        return 1;
    }
    // toString with a length is binary-safe: Lua strings are counted, not
    // NUL-terminated, so payloads may contain embedded zeros.
    bool ok = NetworkManager::Get().sendAppData(reinterpret_cast<const uint8_t*>(data),
                                                static_cast<uint16_t>(len));
    lua.push(ok);
    return 1;
}

int LuaAPI::Net_ReliableQueueDepth(lua_State* L) {
    psyqo::Lua lua(L);
    // Net.ReliableQueueDepth() -> queued, capacity. The reliable channel is
    // stop-and-wait, so a burst can saturate it; compare these before sending a
    // batch rather than discovering the refusal one packet at a time.
    lua.pushNumber(static_cast<int>(NetworkManager::Get().reliableQueueDepth()));
    lua.pushNumber(static_cast<int>(net::NetLink::reliableQueueCapacity()));
    return 2;
}

int LuaAPI::Net_SetPersistent(lua_State* L) {
    psyqo::Lua lua(L);
    // Net.SetPersistent(bool). Keep the network session (our slot, the link)
    // alive across the next Scene.Load(). Scene-scoped bindings - avatar
    // mappings, networked-actor registry - are still dropped, because actorIds
    // mean different objects in a different scene.
    //
    // Call this before Scene.Load() when handing over from a lobby scene to a
    // game scene. Then call Net.Connect() again in the new scene: it re-Hellos
    // with a rebind flag and reclaims the SAME slot instead of joining afresh.
    NetworkManager::Get().setPersistent(lua.toBoolean(1));
    return 0;
}

int LuaAPI::Net_IsPersistent(lua_State* L) {
    psyqo::Lua lua(L);
    lua.push(NetworkManager::Get().isPersistent());
    return 1;
}

void LuaAPI::ResetFrameCount() {
    s_frameCount = 0;
}

void LuaAPI::PushActor(psyqo::Lua& L, uint16_t actorId) {
    L.newTable();
    L.pushNumber(actorId);
    L.setField(-2, "__actor_id");
}

uint16_t LuaAPI::ReadActorId(psyqo::Lua& L, int idx) {
    if (!L.isTable(idx)) return 0xFFFF;
    L.getField(idx, "__actor_id");
    if (!L.isNumber(-1)) {
        L.pop();
        return 0xFFFF;
    }
    uint16_t actorId = static_cast<uint16_t>(L.toNumber(-1));
    L.pop();
    return actorId;
}

// ============================================================================
// ACTOR API IMPLEMENTATION
// ============================================================================

int LuaAPI::Actor_GetPlayer(lua_State* L) {
    psyqo::Lua lua(L);
    PushActor(lua, SceneManager::PLAYER_ACTOR_ID);
    return 1;
}

int LuaAPI::Actor_Find(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = 0xFFFF;
    if (lua.isNumber(1)) {
        actorId = static_cast<uint16_t>(lua.toNumber(1));
        if (!s_sceneManager->isValidActor(actorId)) actorId = 0xFFFF;
    } else if (lua.isString(1)) {
        actorId = s_sceneManager->findActorByName(lua.toString(1));
    }

    if (actorId == 0xFFFF) {
        lua.push();
        return 1;
    }

    PushActor(lua, actorId);
    return 1;
}

int LuaAPI::Actor_FindByIndex(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push();
        return 1;
    }

    uint16_t actorId = static_cast<uint16_t>(lua.toNumber(1));
    if (!s_sceneManager->isValidActor(actorId)) {
        lua.push();
        return 1;
    }

    PushActor(lua, actorId);
    return 1;
}

int LuaAPI::Actor_GetCount(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(s_sceneManager ? static_cast<lua_Number>(s_sceneManager->getActorCount()) : 0);
    return 1;
}

int LuaAPI::Actor_IsPlayer(lua_State* L) {
    psyqo::Lua lua(L);
    uint16_t actorId = ReadActorId(lua, 1);
    lua.push(actorId == SceneManager::PLAYER_ACTOR_ID);
    return 1;
}

int LuaAPI::Actor_GetName(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    const char* name = s_sceneManager->getActorName(actorId);
    if (name) lua.push(name);
    else lua.push();
    return 1;
}

/// Actor.GetPositionXZ(actor) -> x, z as plain integer PIXELS (or nil).
///
/// The allocation-free alternative to Actor.GetPosition, for the 2D case.
///
/// GetPosition costs FOUR Lua tables and three Lua calls every time: one for the
/// vector plus one metatable-carrying FixedPoint per component (psyqo-lua's
/// push(FixedPoint) runs the FixedPoint constructor). A 2D game reading ten
/// avatars' positions each frame was generating well over a thousand tables a
/// second purely to throw them away - and psyqo-lua's collector runs on a 33MHz
/// R3000 with no tuning.
///
/// Every 2D caller then immediately did the same dance on the way out, reading
/// `._raw` and shifting down 12 by hand because FixedPoint's :toNumber() is
/// unreachable (the metatable sets no __index). This returns what they actually
/// wanted in the first place: two numbers, no garbage.
int LuaAPI::Actor_GetPositionXZ(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    psyqo::Vec3 position;
    if (!s_sceneManager->getActorPosition(actorId, position)) {
        lua.push();
        return 1;
    }

    // .integer() truncates toward zero; the Lua helpers this replaces floored.
    // For the screen-plane coordinates this serves, positions are >= 0 in
    // practice and both agree, but do the arithmetic shift explicitly so a map
    // authored across the origin does not shift by a pixel.
    lua.pushNumber(static_cast<lua_Number>(position.x.value >> 12));
    lua.pushNumber(static_cast<lua_Number>(position.z.value >> 12));
    return 2;
}

int LuaAPI::Actor_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    psyqo::Vec3 position;
    if (!s_sceneManager->getActorPosition(actorId, position)) {
        lua.push();
        return 1;
    }

    PushVec3(lua, position.x, position.y, position.z);
    return 1;
}

int LuaAPI::Actor_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || !lua.isTable(2)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 2, x, y, z);
    s_sceneManager->setActorPosition(actorId, psyqo::Vec3{x, y, z});
    return 0;
}

int LuaAPI::Actor_GetRotation(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    psyqo::Vec3 rotation;
    if (!s_sceneManager->getActorRotation(actorId, rotation)) {
        lua.push();
        return 1;
    }

    PushVec3(lua, rotation.x, rotation.y, rotation.z);
    return 1;
}

int LuaAPI::Actor_SetRotation(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || !lua.isTable(2)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 2, x, y, z);
    s_sceneManager->setActorRotation(actorId, psyqo::Vec3{x, y, z});
    return 0;
}

int LuaAPI::Actor_GetEntity(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    GameObject* go = s_sceneManager->getActorGameObject(actorId);
    if (!go) {
        lua.push();
        return 1;
    }

    lua.push(reinterpret_cast<uint8_t*>(go));
    lua.rawGet(LUA_REGISTRYINDEX);
    if (lua.isTable(-1)) return 1;
    lua.pop();
    lua.push();
    return 1;
}

int LuaAPI::Actor_GetNavRegion(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    uint16_t region = s_sceneManager->getActorNavRegion(actorId);
    if (region == NAV_NO_REGION) {
        lua.push();
        return 1;
    }

    lua.pushNumber(region);
    return 1;
}

int LuaAPI::Actor_FindPath(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || lua.isNoneOrNil(2)) {
        lua.push();
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    if (!s_sceneManager->isValidActor(actorId) || !s_sceneManager->isNavLoaded()) {
        lua.push();
        return 1;
    }

    psyqo::Vec3 targetPosition;
    NavPath path;
    bool hasPath = false;

    if (lua.isTable(2)) {
        uint16_t targetActorId = ReadActorId(lua, 2);
        if (targetActorId != 0xFFFF) {
            hasPath = s_sceneManager->findActorPath(actorId, targetActorId, path);
            if (!s_sceneManager->getActorPosition(targetActorId, targetPosition)) {
                lua.push();
                return 1;
            }
        } else {
            ReadVec3(lua, 2, targetPosition.x, targetPosition.y, targetPosition.z);
            hasPath = s_sceneManager->findActorPathToPosition(actorId, targetPosition, path);
        }
    }

    if (!hasPath || path.stepCount <= 0) {
        lua.push();
        return 1;
    }

    lua_newtable(L);
    int waypointIndex = 1;

    for (int i = 1; i < path.stepCount; ++i) {
        psyqo::Vec3 regionCenter;
        if (!s_sceneManager->getNavRegionCenter(path.regions[i], regionCenter)) continue;
        PushVec3(lua, regionCenter.x, regionCenter.y, regionCenter.z);
        lua_rawseti(L, -2, waypointIndex++);
    }

    PushVec3(lua, targetPosition.x, targetPosition.y, targetPosition.z);
    lua_rawseti(L, -2, waypointIndex++);
    return 1;
}

// ============================================================================
// AGENT API IMPLEMENTATION
// ============================================================================

int LuaAPI::Agent_IsAgent(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push(false);
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    lua.push(s_sceneManager->isActorAgent(actorId));
    return 1;
}

int LuaAPI::Agent_SetEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || !lua.isBoolean(2)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    s_sceneManager->setActorAgentEnabled(actorId, lua.toBoolean(2));
    return 0;
}

int LuaAPI::Agent_IsEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push(false);
        return 1;
    }

    uint16_t actorId = ReadActorId(lua, 1);
    lua.push(s_sceneManager->isActorAgentEnabled(actorId));
    return 1;
}

int LuaAPI::Agent_MoveTo(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || lua.isNoneOrNil(2)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    if (lua.isTable(2)) {
        uint16_t targetActorId = ReadActorId(lua, 2);
        if (targetActorId != 0xFFFF) {
            s_sceneManager->moveActorToActor(actorId, targetActorId);
            return 0;
        }

        psyqo::FixedPoint<12> x, y, z;
        ReadVec3(lua, 2, x, y, z);
        s_sceneManager->moveActorToPosition(actorId, psyqo::Vec3{x, y, z});
    }
    return 0;
}

int LuaAPI::Agent_SetTarget(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || !lua.isTable(2)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    uint16_t targetActorId = ReadActorId(lua, 2);
    if (targetActorId != 0xFFFF) {
        s_sceneManager->moveActorToActor(actorId, targetActorId);
    }
    return 0;
}

int LuaAPI::Agent_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    s_sceneManager->stopActor(ReadActorId(lua, 1));
    return 0;
}

int LuaAPI::Agent_IsMoving(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push(false);
        return 1;
    }

    lua.push(s_sceneManager->isActorMoving(ReadActorId(lua, 1)));
    return 1;
}

int LuaAPI::Agent_GetTarget(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t targetActor = 0xFFFF;
    if (!s_sceneManager->getActorTarget(ReadActorId(lua, 1), targetActor)) {
        lua.push();
        return 1;
    }

    PushActor(lua, targetActor);
    return 1;
}

int LuaAPI::Agent_SetSpeed(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    s_sceneManager->setActorMoveSpeed(ReadActorId(lua, 1), readFP(lua, 2));
    return 0;
}

int LuaAPI::Agent_GetSpeed(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.pushNumber(0);
        return 1;
    }

    psyqo::FixedPoint<12> speed = s_sceneManager->getActorMoveSpeed(ReadActorId(lua, 1));
    lua.pushNumber(static_cast<lua_Number>(speed.value) / kFixedScale);
    return 1;
}

// ============================================================================
// AGENT STATE MACHINE IMPLEMENTATIONS
// ============================================================================

int LuaAPI::Agent_GetState(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) { lua.pushNumber(0); return 1; }

    uint16_t actorId = ReadActorId(lua, 1);
    lua.pushNumber(static_cast<int>(s_sceneManager->getActorAgentState(actorId)));
    return 1;
}

int LuaAPI::Agent_SetState(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    int stateInt = static_cast<int>(lua.toNumber(2));
    if (stateInt < 0 || stateInt >= static_cast<int>(psxsplash::SceneManager::AGENT_STATE_COUNT)) return 0;
    s_sceneManager->setActorAgentState(actorId, static_cast<psxsplash::SceneManager::AgentState>(stateInt));
    return 0;
}

int LuaAPI::Agent_CanSee(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) { lua.push(false); return 1; }

    uint16_t observerId = ReadActorId(lua, 1);
    uint16_t targetId   = ReadActorId(lua, 2);
    lua.push(s_sceneManager->canActorSeeActor(observerId, targetId));
    return 1;
}

int LuaAPI::Agent_CanHear(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) { lua.push(false); return 1; }

    uint16_t observerId = ReadActorId(lua, 1);
    uint16_t targetId   = ReadActorId(lua, 2);
    lua.push(s_sceneManager->canActorHearActor(observerId, targetId));
    return 1;
}

int LuaAPI::Agent_SetVisionRange(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    s_sceneManager->setActorVisionRange(actorId, readFP(lua, 2));
    return 0;
}

int LuaAPI::Agent_GetVisionRange(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) { lua.pushNumber(0); return 1; }

    psyqo::FixedPoint<12> r = s_sceneManager->getActorVisionRange(ReadActorId(lua, 1));
    lua.pushNumber(static_cast<lua_Number>(r.value) / kFixedScale);
    return 1;
}

int LuaAPI::Agent_SetVisionAngle(lua_State* L) {
    // Accepts half-angle in degrees (0-180) and stores its fp12 cosine, the same
    // unit the splashpack's visionCosAngle uses. 180 deg sees all around.
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    lua_Number halfAngleDeg = lua.toNumber(2);
    if (halfAngleDeg < 0)   halfAngleDeg = 0;
    if (halfAngleDeg > 180) halfAngleDeg = 180;
    psyqo::Angle halfAngle;
    halfAngle.value = static_cast<int32_t>(halfAngleDeg * 1024 / 180);
    int16_t cosThreshold = static_cast<int16_t>(s_trig.cos(halfAngle).raw());
    s_sceneManager->setActorVisionAngleCos(actorId, cosThreshold);
    return 0;
}

int LuaAPI::Agent_SetHearingRange(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    s_sceneManager->setActorHearingRange(actorId, readFP(lua, 2));
    return 0;
}

int LuaAPI::Agent_GetHearingRange(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) { lua.pushNumber(0); return 1; }

    psyqo::FixedPoint<12> r = s_sceneManager->getActorHearingRange(ReadActorId(lua, 1));
    lua.pushNumber(static_cast<lua_Number>(r.value) / kFixedScale);
    return 1;
}

int LuaAPI::Agent_SetAlertTimeout(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    uint16_t frames  = static_cast<uint16_t>(static_cast<int>(lua.toNumber(2)));
    s_sceneManager->setActorAlertTimeout(actorId, frames);
    return 0;
}

int LuaAPI::Agent_GetLastKnownPos(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) { lua.push(); return 1; }

    uint16_t actorId = ReadActorId(lua, 1);
    psyqo::Vec3 pos;
    if (!s_sceneManager->getActorLastKnownPos(actorId, pos)) {
        lua.push();
        return 1;
    }
    PushVec3(lua, pos.x, pos.y, pos.z);
    return 1;
}

int LuaAPI::Agent_AddWaypoint(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || !lua.isTable(2)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 2, x, y, z);
    s_sceneManager->addActorWaypoint(actorId, psyqo::Vec3{x, y, z});
    return 0;
}

int LuaAPI::Agent_ClearWaypoints(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    s_sceneManager->clearActorWaypoints(ReadActorId(lua, 1));
    return 0;
}

int LuaAPI::Agent_SetPatrolEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    bool enabled     = lua.toBoolean(2);
    s_sceneManager->setActorPatrolEnabled(actorId, enabled);
    return 0;
}

int LuaAPI::Timer_GetFrameCount(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(s_frameCount);
    return 1;
}

// ============================================================================
// CAMERA API IMPLEMENTATION
// ============================================================================

int LuaAPI::Camera_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_sceneManager) {
        auto& pos = s_sceneManager->getCamera().GetPosition();
        PushVec3(lua, pos.x, pos.y, pos.z);
    } else {
        PushVec3(lua, psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0));
    }
    return 1;
}

int LuaAPI::Camera_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isTable(1)) return 0;
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    s_sceneManager->getCamera().SetPosition(x, y, z);
    return 0;
}

int LuaAPI::Camera_GetRotation(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_sceneManager) {
        psyqo::FixedPoint<12> rotX = psyqo::FixedPoint<12>(static_cast<int32_t>(s_sceneManager->getCamera().GetAngleX() * 4), psyqo::FixedPoint<12>::RAW);
        psyqo::FixedPoint<12> rotY = psyqo::FixedPoint<12>(static_cast<int32_t>(s_sceneManager->getCamera().GetAngleY() * 4), psyqo::FixedPoint<12>::RAW);
        psyqo::FixedPoint<12> rotZ = psyqo::FixedPoint<12>(static_cast<int32_t>(s_sceneManager->getCamera().GetAngleZ() * 4), psyqo::FixedPoint<12>::RAW);

        PushVec3(lua, rotX, rotY, rotZ);
    } else {
        // Mirror Camera_GetPosition behavior when no scene manager is available.
        PushVec3(lua, psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0));
    }
    return 1;
}

int LuaAPI::Camera_SetRotation(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isTable(1)) return 0;
    
    // Accept three angles in pi-units (e.g., 0.5 = π/2 = 90°)
    // This matches psyqo::Angle convention used by the engine.
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);

    // Convert to Angle (FixedPoint<10>) 
    psyqo::Angle rx, ry, rz;
    rx.value = x.value >> 2;
    ry.value = y.value >> 2;
    rz.value = z.value >> 2;

    s_sceneManager->getCamera().SetRotation(rx, ry, rz);
    return 0;
}

int LuaAPI::Camera_GetForward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager) {
        psyqo::FixedPoint<12> zero(0);
        PushVec3(lua, zero, zero, zero);
        return 1;
    }
    psyqo::Matrix33 camRotationMatrix = s_sceneManager->getCamera().GetRotation();

    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z;

    PushVec3(lua, fwdX, fwdY, fwdZ);
    return 1;
}

int LuaAPI::Camera_MoveForward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();

    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x * stepAmount;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y * stepAmount;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z * stepAmount;
    
    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x + fwdX;
    pos.y = pos.y + fwdY;
    pos.z = pos.z + fwdZ;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_MoveBackward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();

    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x * stepAmount;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y * stepAmount;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z * stepAmount;
    
    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x - fwdX;
    pos.y = pos.y - fwdY;
    pos.z = pos.z - fwdZ;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_MoveLeft(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();

    // Use the camera's right vector for strafing; negate it to move left.
    psyqo::FixedPoint<12> rightX = camRotationMatrix.vs[0].x * stepAmount;
    psyqo::FixedPoint<12> rightY = camRotationMatrix.vs[0].y * stepAmount;
    psyqo::FixedPoint<12> rightZ = camRotationMatrix.vs[0].z * stepAmount;

    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x - rightX;
    pos.y = pos.y - rightY;
    pos.z = pos.z - rightZ;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_MoveRight(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();

    // Use the camera's right vector for strafing; negate it to move left.
    psyqo::FixedPoint<12> rightX = camRotationMatrix.vs[0].x * stepAmount;
    psyqo::FixedPoint<12> rightY = camRotationMatrix.vs[0].y * stepAmount;
    psyqo::FixedPoint<12> rightZ = camRotationMatrix.vs[0].z * stepAmount;

    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x + rightX;
    pos.y = pos.y + rightY;
    pos.z = pos.z + rightZ;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_FollowPsxPlayer(lua_State* L) {
    psyqo::Lua lua(L);

    if (s_sceneManager && lua.isBoolean(1)) {
        s_sceneManager->setCameraFollowPlayer(lua.toBoolean(1));
        if (lua.toBoolean(1)) {
            s_sceneManager->setCameraFollowActor(SceneManager::PLAYER_ACTOR_ID);
        }
    }
    return 0;
}

int LuaAPI::Camera_SetFollowTarget(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) return 0;

    uint16_t actorId = ReadActorId(lua, 1);
    s_sceneManager->setCameraFollowActor(actorId);
    return 0;
}

int LuaAPI::Camera_GetFollowTarget(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    uint16_t actorId = s_sceneManager->getCameraFollowActor();
    if (!s_sceneManager->isValidActor(actorId)) {
        lua.push();
        return 1;
    }

    PushActor(lua, actorId);
    return 1;
}

int LuaAPI::Camera_ClearFollowTarget(lua_State* L) {
    psyqo::Lua lua(L);
    (void)lua;
    if (s_sceneManager) {
        s_sceneManager->setCameraFollowPlayer(false);
    }
    return 0;
}

int LuaAPI::Camera_LookAt(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager) return 0;
    
    psyqo::FixedPoint<12> tx, ty, tz;
    
    if (lua.isTable(1)) {
        ReadVec3(lua, 1, tx, ty, tz);
    } else {
        tx = lua.isNoneOrNil(1) ? psyqo::FixedPoint<12>() : readFP(lua, 1);
        ty = lua.isNoneOrNil(2) ? psyqo::FixedPoint<12>() : readFP(lua, 2);
        tz = lua.isNoneOrNil(3) ? psyqo::FixedPoint<12>() : readFP(lua, 3);
    }
    
    auto& cam = s_sceneManager->getCamera();
    auto& pos = cam.GetPosition();
    
    // Compute direction vector from camera to target
    auto dx = tx - pos.x;
    auto dy = ty - pos.y;
    auto dz = tz - pos.z;

    // Horizontal distance, for pitch. SoftMath::squareRoot keeps the fixed-point
    // scale, so its result shares units with dy and atan2 sees a consistent pair.
    auto horizDist = psyqo::SoftMath::squareRoot(dx * dx + dz * dz);

    // Yaw: the engine's own forward vector is (sin(yaw), *, cos(yaw)) - see the
    // line-of-sight direction in SceneManager - so yaw is atan2(dx, dz).
    // Pitch: look-down decrements playerRotationX in Controls, so positive pitch
    // is up. World Y points down, so a target above the camera has dy < 0 and
    // wants atan2(-dy, horizDist).
    psyqo::Angle yaw = psyqo::atan2(dx.raw(), dz.raw());
    psyqo::Angle pitch = psyqo::atan2(-dy.raw(), horizDist.raw());

    // Same entry point first-person aiming uses, so the result stays consistent
    // with the player-driven camera by construction.
    cam.SetRotation(pitch, yaw, psyqo::Angle());
    return 0;
}

int LuaAPI::Camera_GetH(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) { lua.pushNumber(120); return 1; }
    lua.pushNumber(static_cast<lua_Number>(s_sceneManager->getCamera().GetProjectionH()));
    return 1;
}

int LuaAPI::Camera_SetH(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    int32_t h = static_cast<int32_t>(lua.toNumber(1));
    if (h < 1) h = 1;
    if (h > 1024) h = 1024;
    s_sceneManager->getCamera().SetProjectionH(h);
    return 0;
}

// ============================================================================
// AUDIO API IMPLEMENTATION
// ============================================================================

int LuaAPI::Audio_Play(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager) {
        lua.pushNumber(-1);
        return 1;
    }

    int soundId = -1;

    // Accept number (index) or string (name lookup) like Entity.Find
    // Check isNumber FIRST - in Lua, numbers pass isString too.
    if (lua.isNumber(1)) {
        soundId = static_cast<int>(lua.toNumber(1));
    } else if (lua.isString(1)) {
        const char* name = lua.toString(1);
        soundId = s_sceneManager->findAudioClipByName(name);
        if (soundId < 0) {
            lua.pushNumber(-1);
            return 1;
        }
    } else {
        lua.pushNumber(-1);
        return 1;
    }

    int volume = static_cast<int>(lua.optNumber(2, 100));
    int pan = static_cast<int>(lua.optNumber(3, 64));

    int voice = s_sceneManager->getAudio().play(soundId, volume, pan);
    lua.pushNumber(voice);
    return 1;
}

int LuaAPI::Audio_Find(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isString(1)) {
        lua.push();  // nil
        return 1;
    }
    
    const char* name = lua.toString(1);
    int clipIndex = s_sceneManager->findAudioClipByName(name);
    
    if (clipIndex >= 0) {
        lua.pushNumber(static_cast<lua_Number>(clipIndex));
    } else {
        lua.push();  // nil
    }
    return 1;
}

int LuaAPI::Audio_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    int channelId = static_cast<int>(lua.toNumber(1));
    s_sceneManager->getAudio().stopVoice(channelId);
    return 0;
}

int LuaAPI::Audio_SetVolume(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    int channelId = static_cast<int>(lua.toNumber(1));
    int volume = static_cast<int>(lua.toNumber(2));
    int pan = static_cast<int>(lua.optNumber(3, 64));
    s_sceneManager->getAudio().setVoiceVolume(channelId, volume, pan);
    return 0;
}

int LuaAPI::Audio_StopAll(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    s_sceneManager->getAudio().stopAll();
    return 0;
}

int LuaAPI::Audio_PlayCDDA(lua_State *L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    s_sceneManager->getMusic().playCDDATrack(static_cast<int>(lua.toNumber(1)));
    return 0;
}

int LuaAPI::Audio_ResumeCDDA(lua_State *L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    s_sceneManager->getMusic().resumeCDDA();
    return 0;
}

int LuaAPI::Audio_PauseCDDA(lua_State *L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    s_sceneManager->getMusic().pauseCDDA();
    return 0;
}

int LuaAPI::Audio_StopCDDA(lua_State *L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    s_sceneManager->getMusic().stopCDDA();
    return 0;
}

int LuaAPI::Audio_TellCDDA(lua_State *L) {
    if (!s_sceneManager) return 0;
    s_sceneManager->getMusic().tellCDDA(L);
    return 0;
}

int LuaAPI::Audio_SetCDDAVolume(lua_State *L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    s_sceneManager->getMusic().setCDDAVolume(static_cast<int>(lua.toNumber(1)), static_cast<int>(lua.toNumber(2)));
    return 0;
}

// ============================================================================
// DEBUG API IMPLEMENTATION
// ============================================================================

int LuaAPI::Debug_Log(lua_State* L) {
    psyqo::Lua lua(L);
    if (lua.isString(1)) {
        printf("%s\n", lua.toString(1));
    }
    return 0;
}

int LuaAPI::Debug_DrawLine(lua_State* L) {
    psyqo::Lua lua(L);
    
    // Parse start and end Vec3 tables, optional color
    psyqo::FixedPoint<12> sx, sy, sz, ex, ey, ez;
    if (lua.isTable(1) && lua.isTable(2)) {
        ReadVec3(lua, 1, sx, sy, sz);
        ReadVec3(lua, 2, ex, ey, ez);
    }
    
    // TODO: Queue LINE_G2 primitive through Renderer
    return 0;
}

int LuaAPI::Debug_DrawBox(lua_State* L) {
    psyqo::Lua lua(L);
    
    // Parse center and size Vec3 tables, optional color
    psyqo::FixedPoint<12> cx, cy, cz, hx, hy, hz;
    if (lua.isTable(1) && lua.isTable(2)) {
        ReadVec3(lua, 1, cx, cy, cz);
        ReadVec3(lua, 2, hx, hy, hz);
    }
    
    // TODO: Queue 12 LINE_G2 primitives (box wireframe) through Renderer
    return 0;
}

// ============================================================================
// FP API IMPLEMENTATION
// ============================================================================

int LuaAPI::Convert_IntToFp(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isNumber(1)) {
        return 0;
    }

    psyqo::FixedPoint<12> numberFp; 
    numberFp = psyqo::FixedPoint<12>(static_cast<int32_t>(lua.toNumber(1)), psyqo::FixedPoint<12>::RAW);
    
    lua.push(numberFp);
    return 1;
}

int LuaAPI::Convert_FpToInt(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!IsFixedPointSafe(lua, 1)) {
        return 0;
    }

    // raw() is int32_t. Holding it in a uint32_t turned every negative fixed
    // point into a large positive one. Convert_IntToFp is the matching RAW
    // pass-through, so this stays raw() rather than becoming integer().
    int32_t numberInt = lua.toFixedPoint(1).raw();

    lua.pushNumber(numberInt);
    return 1;
}


// ============================================================================
// MATH API IMPLEMENTATION
// ============================================================================

int LuaAPI::Math_Clamp(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number value = lua.toNumber(1);
    lua_Number minVal = lua.toNumber(2);
    lua_Number maxVal = lua.toNumber(3);
    
    if (value < minVal) value = minVal;
    if (value > maxVal) value = maxVal;
    
    lua.pushNumber(value);
    return 1;
}

int LuaAPI::Math_Lerp(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number a = lua.toNumber(1);
    lua_Number b = lua.toNumber(2);
    lua_Number t = lua.toNumber(3);
    
    lua.pushNumber(a + (b - a) * t);
    return 1;
}

int LuaAPI::Math_Sign(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number value = lua.toNumber(1);
    
    if (value > 0) lua.pushNumber(1);
    else if (value < 0) lua.pushNumber(-1);
    else lua.pushNumber(0);
    
    return 1;
}

int LuaAPI::Math_Abs(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number value = lua.toNumber(1);
    lua.pushNumber(value < 0 ? -value : value);
    return 1;
}

int LuaAPI::Math_Min(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number a = lua.toNumber(1);
    lua_Number b = lua.toNumber(2);
    
    lua.pushNumber(a < b ? a : b);
    return 1;
}

int LuaAPI::Math_Max(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number a = lua.toNumber(1);
    lua_Number b = lua.toNumber(2);
    
    lua.pushNumber(a > b ? a : b);
    return 1;
}

int LuaAPI::Math_Cos(lua_State* L)
{
    psyqo::Lua lua(L);

    if (!lua.isNumber(1))
    {
        return 0;
    }

    int value = (int)lua.toNumber(1);
    psyqo::Angle angle = ((uint32_t)value * (2.0_pi)) / 360;

    auto result = s_trig.cos(angle);

    lua.push(result);

    return 1;
}

int LuaAPI::Math_Sin(lua_State* L)
{
    psyqo::Lua lua(L);

    if (!lua.isNumber(1))
    {
        return 0;
    }

    int value = (int)lua.toNumber(1);
    psyqo::Angle angle = ((uint32_t)value * (2.0_pi)) / 360;

    auto result = s_trig.sin(angle);

    lua.push(result);

    return 1;
}

// Copied directly from renderer.cpp
static inline void worldToCamera(int32_t wx, int32_t wy, int32_t wz,
    int32_t camX, int32_t camY, int32_t camZ,
    const psyqo::Matrix33& camRot,
    int32_t& outX, int32_t& outY, int32_t& outZ) {
    int32_t rx = wx - camX, ry = wy - camY, rz = wz - camZ;
    outX = (int32_t)(((int64_t)camRot.vs[0].x.value * rx + (int64_t)camRot.vs[0].y.value * ry +
        (int64_t)camRot.vs[0].z.value * rz) >> 12);
    outY = (int32_t)(((int64_t)camRot.vs[1].x.value * rx + (int64_t)camRot.vs[1].y.value * ry +
        (int64_t)camRot.vs[1].z.value * rz) >> 12);
    outZ = (int32_t)(((int64_t)camRot.vs[2].x.value * rx + (int64_t)camRot.vs[2].y.value * ry +
        (int64_t)camRot.vs[2].z.value * rz) >> 12);
}

// Copied directly from renderer.cpp
// (The bit-shifts on rawX and rawY were scrapped to allow for more precision)
static inline bool projectToScreen(int32_t vx, int32_t vy, int32_t vz,
    int32_t projH, int16_t& sx, int16_t& sy) {
    if (vz <= 0) return false;
    int32_t vzs = vz >> 4; if (vzs <= 0) vzs = 1;
    int32_t rawX = ((vx * projH) / vz) + 160;
    int32_t rawY = ((vy * projH) / vz) + 120;
    if (rawX < -2048) rawX = -2048; else if (rawX > 2048) rawX = 2048;
    if (rawY < -2048) rawY = -2048; else if (rawY > 2048) rawY = 2048;
    sx = (int16_t)rawX;
    sy = (int16_t)rawY;
    return true;
}

static inline bool projectWorldToScreen(
    int32_t wx, int32_t wy, int32_t wz,
    const psyqo::Vec3& camPos,
    const psyqo::Matrix33& camRot,
    int32_t projH,
    int16_t& outX, int16_t& outY)
{
    int32_t vx, vy, vz;
    worldToCamera(wx, wy, wz,
        camPos.x.raw(), camPos.y.raw(), camPos.z.raw(),
        camRot, vx, vy, vz);

    if (vz <= 0) {
        outX = 0;
        outY = 0;
        return false;
    }

    return projectToScreen(vx, vy, vz, projH, outX, outY);
}

int LuaAPI::Math_Convert3DTo2D(lua_State* L)
{
    psyqo::Lua lua(L);

    if (!lua.isTable(1))
    {
        return 0;
    }

    psyqo::FixedPoint<12> pos_x, pos_y, pos_z;
    ReadVec3(lua, 1, pos_x, pos_y, pos_z);

    auto& cam = s_sceneManager->getCamera();
    psyqo::Vec3 camPos = cam.GetPosition();
    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();
    int32_t projH = cam.GetProjectionH();

    int16_t screenX = 0;
    int16_t screenY = 0;

    bool visible = projectWorldToScreen(
        pos_x.raw(), pos_y.raw(), pos_z.raw(),
        camPos, camRotationMatrix, projH,
        screenX, screenY);

    lua.pushNumber(screenX);
    lua.pushNumber(screenY);
    // Third return, so a script can tell a point behind the camera from one that
    // genuinely projects to (0,0). Extra results are discarded by callers that
    // only unpack two, so existing scripts are unaffected.
    lua.push(visible);

    return 3;
}

// ============================================================================
// RANDOM API IMPLEMENTATION
// ============================================================================

int LuaAPI::Random_Number(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        return 0;
    }

    uint32_t max = lua.toNumber(1);
    uint32_t value = s_sceneManager->m_random.number(max)+1;

    lua.pushNumber(value);
    return 1;
}

int LuaAPI::Random_GeneratorNumber(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        return 0;
    }

    uint32_t max = lua.toNumber(1);
    uint32_t value = s_sceneManager->m_randomGenerator.number(max)+1;

    lua.pushNumber(value);
    return 1;
}

int LuaAPI::Random_Range(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1) || !lua.isNumber(2)) {
        return 0;
    }

    int32_t min = lua.toNumber(1);
    int32_t max = lua.toNumber(2);
    // Reversed bounds used to wrap: min == max + 1 makes difference 0xFFFFFFFF
    // and difference + 1 exactly 0, so number() divided by zero. Bounds are
    // signed, so a range spanning zero must be compared as such.
    if (min > max) {
        int32_t t = min;
        min = max;
        max = t;
    }
    uint32_t difference = uint32_t(max) - uint32_t(min);

    uint32_t value = s_sceneManager->m_random.number(difference+1) + min;

    lua.pushNumber(value);
    return 1;
}

int LuaAPI::Random_GeneratorRange(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1) || !lua.isNumber(2)) {
        return 0;
    }
    int32_t min = lua.toNumber(1);
    int32_t max = lua.toNumber(2);
    if (min > max) {
        int32_t t = min;
        min = max;
        max = t;
    }
    uint32_t difference = uint32_t(max) - uint32_t(min);

    uint32_t value = s_sceneManager->m_randomGenerator.number(difference+1) + min;

    lua.pushNumber(value);
    return 1;
}

int LuaAPI::Random_GeneratorSeed(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isNumber(1)) {
        return 0;
    }

    uint32_t newSeed = static_cast<uint32_t>(lua.toNumber(1));

    if(newSeed == 0){
        newSeed = 108;
    }

    s_sceneManager->m_randomGenerator.seed(newSeed);

    return 0;
}

// ============================================================================
// SCENE API IMPLEMENTATION
// ============================================================================

int LuaAPI::Scene_Load(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        return 0;
    }
    
    int sceneIndex = static_cast<int>(lua.toNumber(1));
    s_sceneManager->requestSceneLoad(sceneIndex);
    return 0;
}

int LuaAPI::Scene_GetIndex(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager) {
        lua.pushNumber(0);
        return 1;
    }
    
    lua.pushNumber(static_cast<lua_Number>(s_sceneManager->getCurrentSceneIndex()));
    return 1;
}

// ============================================================================
// PERSIST API IMPLEMENTATION
// ============================================================================

struct PersistEntry {
    char key[32];
    lua_Number value;
    bool used;
};

static PersistEntry s_persistData[16] = {};

// Inline string helpers (no libc on bare-metal PS1)
static bool streq(const char* a, const char* b) {
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

static void strcopy(char* dst, const char* src, int maxLen) {
    int i = 0;
    for (; i < maxLen - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

int LuaAPI::Persist_Get(lua_State* L) {
    psyqo::Lua lua(L);
    const char* key = lua.toString(1);
    if (!key) { lua.push(); return 1; }
    
    for (int i = 0; i < 16; i++) {
        if (s_persistData[i].used && streq(s_persistData[i].key, key)) {
            lua.pushNumber(s_persistData[i].value);
            return 1;
        }
    }
    lua.push();  // nil
    return 1;
}

int LuaAPI::Persist_Set(lua_State* L) {
    psyqo::Lua lua(L);
    const char* key = lua.toString(1);
    if (!key) return 0;
    
    lua_Number value = lua.toNumber(2);
    
    // Update existing key
    for (int i = 0; i < 16; i++) {
        if (s_persistData[i].used && streq(s_persistData[i].key, key)) {
            s_persistData[i].value = value;
            return 0;
        }
    }
    
    // Find empty slot
    for (int i = 0; i < 16; i++) {
        if (!s_persistData[i].used) {
            strcopy(s_persistData[i].key, key, 32);
            s_persistData[i].value = value;
            s_persistData[i].used = true;
            return 0;
        }
    }
    
    return 0;  // No room — silently fail
}

void LuaAPI::PersistClear() {
    for (int i = 0; i < 16; i++) {
        s_persistData[i].used = false;
    }
}

// ============================================================================
// MEMCARD API IMPLEMENTATION
// ============================================================================

// Reads the port argument (0 or 1) at stack index 1. On an invalid port, pushes
// the standard (failValue, errString) pair and returns false.
static bool memcardReadPort(psyqo::Lua& lua, MemoryCardManager::Port& outPort, bool failIsBool) {
    int index = static_cast<int>(lua.toNumber(1));
    if (MemoryCardManager::portFromIndex(index, &outPort)) return true;
    if (failIsBool) {
        lua.push(false);
    } else {
        lua.push();  // nil
    }
    lua.push("invalid memory card port (use 0 or 1)");
    return false;
}

int LuaAPI::MemCard_IsPresent(lua_State* L) {
    psyqo::Lua lua(L);
    MemoryCardManager::Port port;
    if (!memcardReadPort(lua, port, /*failIsBool=*/true)) return 2;

    bool present = false;
    const char* err = MemoryCardManager::Get().isPresent(port, &present);
    if (err) {
        lua.push(false);
        lua.push(eastl::string_view(err));
        return 2;
    }
    lua.push(present);
    lua.push();
    return 2;
}

int LuaAPI::MemCard_Format(lua_State* L) {
    psyqo::Lua lua(L);
    MemoryCardManager::Port port;
    if (!memcardReadPort(lua, port, /*failIsBool=*/true)) return 2;

    const char* err = MemoryCardManager::Get().format(port);
    if (err) {
        lua.push(false);
        lua.push(eastl::string_view(err));
        return 2;
    }
    lua.push(true);
    lua.push();
    return 2;
}

int LuaAPI::MemCard_Save(lua_State* L) {
    psyqo::Lua lua(L);
    MemoryCardManager::Port port;
    if (!memcardReadPort(lua, port, /*failIsBool=*/true)) return 2;

    const char* key = lua.toString(2);
    if (!key) {
        lua.push(false);
        lua.push("MemCard.Save: missing save key");
        return 2;
    }
    if (!lua.isTable(3)) {
        lua.push(false);
        lua.push("MemCard.Save: third argument must be a table");
        return 2;
    }
    const char* title = lua.isString(4) ? lua.toString(4) : nullptr;

    const char* err = MemoryCardManager::Get().save(port, key, title, lua, 3);
    if (err) {
        lua.push(false);
        lua.push(eastl::string_view(err));
        return 2;
    }
    lua.push(true);
    lua.push();
    return 2;
}

int LuaAPI::MemCard_Load(lua_State* L) {
    psyqo::Lua lua(L);
    MemoryCardManager::Port port;
    if (!memcardReadPort(lua, port, /*failIsBool=*/false)) return 2;

    const char* key = lua.toString(2);
    if (!key) {
        lua.push();
        lua.push("MemCard.Load: missing save key");
        return 2;
    }

    int top = lua.getTop();
    const char* err = MemoryCardManager::Get().load(port, key, lua);
    if (err) {
        lua.setTop(top);  // discard any partially-pushed values
        lua.push();
        lua.push(eastl::string_view(err));
        return 2;
    }
    // On success the loaded value sits on top of the stack.
    lua.push();  // nil error
    return 2;
}

int LuaAPI::MemCard_Delete(lua_State* L) {
    psyqo::Lua lua(L);
    MemoryCardManager::Port port;
    if (!memcardReadPort(lua, port, /*failIsBool=*/true)) return 2;

    const char* key = lua.toString(2);
    if (!key) {
        lua.push(false);
        lua.push("MemCard.Delete: missing save key");
        return 2;
    }
    const char* err = MemoryCardManager::Get().remove(port, key);
    if (err) {
        lua.push(false);
        lua.push(eastl::string_view(err));
        return 2;
    }
    lua.push(true);
    lua.push();
    return 2;
}

int LuaAPI::MemCard_List(lua_State* L) {
    psyqo::Lua lua(L);
    MemoryCardManager::Port port;
    if (!memcardReadPort(lua, port, /*failIsBool=*/false)) return 2;

    psyqo::MemoryCardFileSystem::FileEntry entries[15];
    uint32_t count = 0;
    const char* err = MemoryCardManager::Get().listFiles(port, entries, 15, &count);
    if (err) {
        lua.push();
        lua.push(eastl::string_view(err));
        return 2;
    }
    lua.newTable();
    int table = lua.getTop();
    uint32_t shown = count < 15 ? count : 15;
    for (uint32_t i = 0; i < shown; i++) {
        lua.push(eastl::string_view(entries[i].name));
        lua.rawSetI(table, static_cast<int>(i + 1));
    }
    lua.push();  // nil error
    return 2;
}

int LuaAPI::MemCard_FreeBlocks(lua_State* L) {
    psyqo::Lua lua(L);
    MemoryCardManager::Port port;
    if (!memcardReadPort(lua, port, /*failIsBool=*/false)) return 2;

    uint32_t blocks = 0;
    const char* err = MemoryCardManager::Get().freeBlocks(port, &blocks);
    if (err) {
        lua.push();
        lua.push(eastl::string_view(err));
        return 2;
    }
    lua.pushNumber(static_cast<lua_Number>(blocks));
    lua.push();
    return 2;
}

// ============================================================================
// CUTSCENE API IMPLEMENTATION
// ============================================================================

int LuaAPI::Cutscene_Play(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_cutscenePlayer || !lua.isString(1)) {
        return 0;
    }

    const char* name = lua.toString(1);
    bool loop = false;
    int onCompleteRef = LUA_NOREF;

    // Optional second argument: options table {loop=bool, onComplete=function}
    if (lua.isTable(2)) {
        lua.getField(2, "loop");
        if (lua.isBoolean(-1)) loop = lua.toBoolean(-1);
        lua.pop();

        lua.getField(2, "onComplete");
        if (lua.isFunction(-1)) {
            onCompleteRef = lua.ref();  // pops and stores in registry
        } else {
            lua.pop();
        }
    }

    // Clear any previous callback before starting a new cutscene
    int oldRef = s_cutscenePlayer->getOnCompleteRef();
    if (oldRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, oldRef);
    }

    s_cutscenePlayer->setLuaState(L);
    s_cutscenePlayer->setOnCompleteRef(onCompleteRef);
    s_cutscenePlayer->play(name, loop);
    return 0;
}

int LuaAPI::Cutscene_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_cutscenePlayer) {
        s_cutscenePlayer->stop();
    }
    return 0;
}

int LuaAPI::Cutscene_IsPlaying(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_cutscenePlayer) {
        lua.push(s_cutscenePlayer->isPlaying());
    } else {
        lua.push(false);
    }
    return 1;
}

// ============================================================================
// ANIMATION API IMPLEMENTATION
// ============================================================================

int LuaAPI::Animation_Play(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_animationPlayer || !lua.isString(1)) {
        return 0;
    }

    const char* name = lua.toString(1);
    bool loop = false;
    int onCompleteRef = LUA_NOREF;

    if (lua.isTable(2)) {
        lua.getField(2, "loop");
        if (lua.isBoolean(-1)) loop = lua.toBoolean(-1);
        lua.pop();

        lua.getField(2, "onComplete");
        if (lua.isFunction(-1)) {
            onCompleteRef = lua.ref();  // pops and stores in registry
        } else {
            lua.pop();
        }
    }

    s_animationPlayer->setLuaState(L);
    s_animationPlayer->play(name, loop);

    if (onCompleteRef != LUA_NOREF) {
        s_animationPlayer->setOnCompleteRef(name, onCompleteRef);
    }

    return 0;
}

int LuaAPI::Animation_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_animationPlayer) return 0;

    if (lua.isString(1)) {
        s_animationPlayer->stop(lua.toString(1));
    } else {
        s_animationPlayer->stopAll();
    }
    return 0;
}

int LuaAPI::Animation_IsPlaying(lua_State* L) {
    psyqo::Lua lua(L);

    if (s_animationPlayer && lua.isString(1)) {
        lua.push(s_animationPlayer->isPlaying(lua.toString(1)));
    } else {
        lua.push(false);
    }
    return 1;
}

// ============================================================================
// SKINNED ANIMATION API IMPLEMENTATION
// ============================================================================

int LuaAPI::SkinnedAnim_Play(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isString(1) || !lua.isString(2)) return 0;

    const char* objectName = lua.toString(1);
    const char* clipName = lua.toString(2);

    int si = s_sceneManager->findSkinAnimByObjectName(objectName);
    if (si < 0) return 0;

    SkinAnimSet& animSet = s_sceneManager->getSkinAnimSet(si);
    SkinAnimState& animState = s_sceneManager->getSkinAnimState(si);

    // Find clip by name
    int clipIdx = -1;
    for (int ci = 0; ci < animSet.clipCount; ci++) {
        if (animSet.clips[ci].name && streq(animSet.clips[ci].name, clipName)) {
            clipIdx = ci;
            break;
        }
    }
    if (clipIdx < 0) return 0;

    bool loop = false;
    int onCompleteRef = LUA_NOREF;

    if (lua.isTable(3)) {
        lua.getField(3, "loop");
        if (lua.isBoolean(-1)) loop = lua.toBoolean(-1);
        lua.pop();

        lua.getField(3, "onComplete");
        if (lua.isFunction(-1)) {
            onCompleteRef = lua.ref();  // pops and stores in registry
        } else {
            lua.pop();
        }
    }

    animState.currentClip = (uint8_t)clipIdx;
    animState.currentFrame = 0;
    animState.subFrame = 0;
    animState.playing = true;
    animState.loop = loop;

    // Release old callback if any
    if (animState.luaCallbackRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, animState.luaCallbackRef);
    }
    animState.luaCallbackRef = onCompleteRef;

    return 0;
}

int LuaAPI::SkinnedAnim_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isString(1)) return 0;

    int si = s_sceneManager->findSkinAnimByObjectName(lua.toString(1));
    if (si < 0) return 0;

    SkinAnimState& animState = s_sceneManager->getSkinAnimState(si);
    animState.playing = false;

    // Release callback
    if (animState.luaCallbackRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, animState.luaCallbackRef);
        animState.luaCallbackRef = LUA_NOREF;
    }

    return 0;
}

int LuaAPI::SkinnedAnim_IsPlaying(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isString(1)) {
        lua.push(false);
        return 1;
    }

    int si = s_sceneManager->findSkinAnimByObjectName(lua.toString(1));
    if (si < 0) {
        lua.push(false);
        return 1;
    }

    lua.push(s_sceneManager->getSkinAnimState(si).playing);
    return 1;
}

int LuaAPI::SkinnedAnim_GetClip(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isString(1)) {
        lua_pushnil(L);
        return 1;
    }

    int si = s_sceneManager->findSkinAnimByObjectName(lua.toString(1));
    if (si < 0) {
        lua_pushnil(L);
        return 1;
    }

    const SkinAnimSet& animSet = s_sceneManager->getSkinAnimSet(si);
    const SkinAnimState& animState = s_sceneManager->getSkinAnimState(si);

    if (animState.currentClip < animSet.clipCount &&
        animSet.clips[animState.currentClip].name) {
        lua.push(animSet.clips[animState.currentClip].name);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ============================================================================
// CONTROLS API IMPLEMENTATION
// ============================================================================

int LuaAPI::Controls_SetEnabledPlayer1(lua_State* L) {
    psyqo::Lua lua(L);
    if (s_sceneManager && lua.isBoolean(1)) {
        s_sceneManager->setControlsEnabledPlayer1(lua.toBoolean(1));
    }
    return 0;
}

int LuaAPI::Controls_SetEnabledPlayer2(lua_State* L) {
    psyqo::Lua lua(L);
    if (s_sceneManager && lua.isBoolean(1)) {
        s_sceneManager->setControlsEnabledPlayer2(lua.toBoolean(1));
    }
    return 0;
}

int LuaAPI::Controls_IsEnabledPlayer1(lua_State* L) {
    psyqo::Lua lua(L);
    if (s_sceneManager) {
        lua.push(s_sceneManager->isControlsEnabledPlayer1());
    } else {
        lua.push(false);
    }
    return 1;
}

int LuaAPI::Controls_IsEnabledPlayer2(lua_State* L) {
    psyqo::Lua lua(L);
    if (s_sceneManager) {
        lua.push(s_sceneManager->isControlsEnabledPlayer2());
    }
    else {
        lua.push(false);
    }
    return 1;
}

// ============================================================================
// INTERACT API IMPLEMENTATION
// ============================================================================

int LuaAPI::Interact_SetEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || !lua.isBoolean(2)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<psxsplash::GameObject>(-1);
    lua.pop();

    if (go && go->hasInteractable()) {
        auto* inter = s_sceneManager->getInteractable(go->interactableIndex);
        if (inter) {
            inter->setDisabled(!lua.toBoolean(2));
        }
    }
    return 0;
}

int LuaAPI::Interact_IsEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) {
        lua.push(false);
        return 1;
    }

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<psxsplash::GameObject>(-1);
    lua.pop();

    if (go && go->hasInteractable()) {
        auto* inter = s_sceneManager->getInteractable(go->interactableIndex);
        if (inter) {
            lua.push(!inter->isDisabled());
            return 1;
        }
    }
    lua.push(false);
    return 1;
}

// ============================================================================
// UI API IMPLEMENTATION
// ============================================================================

int LuaAPI::UI_FindCanvas(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isString(1)) {
        lua.pushNumber(-1);
        return 1;
    }
    const char* name = lua.toString(1);
    int idx = s_uiSystem->findCanvas(name);
    lua.pushNumber(static_cast<lua_Number>(idx));
    return 1;
}

int LuaAPI::UI_SetCanvasVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem) return 0;
    int idx;
    // Accept number (index) or string (name)
    if (lua.isNumber(1)) {
        idx = static_cast<int>(lua.toNumber(1));
    } else if (lua.isString(1)) {
        idx = s_uiSystem->findCanvas(lua.toString(1));
    } else {
        return 0;
    }
    bool visible = lua.toBoolean(2);
    s_uiSystem->setCanvasVisible(idx, visible);
    return 0;
}

int LuaAPI::UI_IsCanvasVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem) {
        lua.push(false);
        return 1;
    }
    int idx;
    if (lua.isNumber(1)) {
        idx = static_cast<int>(lua.toNumber(1));
    } else if (lua.isString(1)) {
        idx = s_uiSystem->findCanvas(lua.toString(1));
    } else {
        lua.push(false);
        return 1;
    }
    lua.push(s_uiSystem->isCanvasVisible(idx));
    return 1;
}

int LuaAPI::UI_FindElement(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1) || !lua.isString(2)) {
        lua.pushNumber(-1);
        return 1;
    }
    int canvasIdx = static_cast<int>(lua.toNumber(1));
    const char* name = lua.toString(2);
    int handle = s_uiSystem->findElement(canvasIdx, name);
    lua.pushNumber(static_cast<lua_Number>(handle));
    return 1;
}

int LuaAPI::UI_SetVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    bool visible = lua.toBoolean(2);
    s_uiSystem->setElementVisible(handle, visible);
    return 0;
}

int LuaAPI::UI_IsVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.push(s_uiSystem->isElementVisible(handle));
    return 1;
}

int LuaAPI::UI_SetText(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    const char* text = lua.isString(2) ? lua.toString(2) : "";
    s_uiSystem->setText(handle, text);
    return 0;
}

int LuaAPI::UI_SetProgress(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    int value = static_cast<int>(lua.toNumber(2));
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    s_uiSystem->setProgress(handle, (uint8_t)value);
    return 0;
}

int LuaAPI::UI_GetProgress(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0);
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.pushNumber(static_cast<lua_Number>(s_uiSystem->getProgress(handle)));
    return 1;
}

int LuaAPI::UI_SetColor(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    uint8_t r = static_cast<uint8_t>(lua.toNumber(2));
    uint8_t g = static_cast<uint8_t>(lua.toNumber(3));
    uint8_t b = static_cast<uint8_t>(lua.toNumber(4));
    s_uiSystem->setColor(handle, r, g, b);
    return 0;
}

int LuaAPI::UI_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t x = static_cast<int16_t>(lua.toNumber(2));
    int16_t y = static_cast<int16_t>(lua.toNumber(3));
    s_uiSystem->setPosition(handle, x, y);
    return 0;
}

int LuaAPI::UI_GetText(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.push("");
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.push(s_uiSystem->getText(handle));
    return 1;
}

int LuaAPI::UI_GetColor(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0); lua.pushNumber(0); lua.pushNumber(0);
        return 3;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    uint8_t r, g, b;
    s_uiSystem->getColor(handle, r, g, b);
    lua.pushNumber(r); lua.pushNumber(g); lua.pushNumber(b);
    return 3;
}

int LuaAPI::UI_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0); lua.pushNumber(0);
        return 2;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t x, y;
    s_uiSystem->getPosition(handle, x, y);
    lua.pushNumber(static_cast<lua_Number>(x));
    lua.pushNumber(static_cast<lua_Number>(y));
    return 2;
}

int LuaAPI::UI_SetSize(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t w = static_cast<int16_t>(lua.toNumber(2));
    int16_t h = static_cast<int16_t>(lua.toNumber(3));
    s_uiSystem->setSize(handle, w, h);
    return 0;
}

int LuaAPI::UI_GetSize(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0); lua.pushNumber(0);
        return 2;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t w, h;
    s_uiSystem->getSize(handle, w, h);
    lua.pushNumber(static_cast<lua_Number>(w));
    lua.pushNumber(static_cast<lua_Number>(h));
    return 2;
}

int LuaAPI::UI_SetProgressColors(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    uint8_t bgR = static_cast<uint8_t>(lua.toNumber(2));
    uint8_t bgG = static_cast<uint8_t>(lua.toNumber(3));
    uint8_t bgB = static_cast<uint8_t>(lua.toNumber(4));
    uint8_t fR  = static_cast<uint8_t>(lua.toNumber(5));
    uint8_t fG  = static_cast<uint8_t>(lua.toNumber(6));
    uint8_t fB  = static_cast<uint8_t>(lua.toNumber(7));
    s_uiSystem->setProgressColors(handle, bgR, bgG, bgB, fR, fG, fB);
    return 0;
}

int LuaAPI::UI_GetElementType(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(-1);
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.pushNumber(static_cast<lua_Number>(static_cast<uint8_t>(s_uiSystem->getElementType(handle))));
    return 1;
}

int LuaAPI::UI_GetElementCount(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0);
        return 1;
    }
    int canvasIdx = static_cast<int>(lua.toNumber(1));
    lua.pushNumber(static_cast<lua_Number>(s_uiSystem->getCanvasElementCount(canvasIdx)));
    return 1;
}

int LuaAPI::UI_GetElementByIndex(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1) || !lua.isNumber(2)) {
        lua.pushNumber(-1);
        return 1;
    }
    int canvasIdx = static_cast<int>(lua.toNumber(1));
    int elemIdx = static_cast<int>(lua.toNumber(2));
    int handle = s_uiSystem->getCanvasElementHandle(canvasIdx, elemIdx);
    lua.pushNumber(static_cast<lua_Number>(handle));
    return 1;
}

/*void renderLine(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
    psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc)
{

}*/

// parameters: UI_DrawLine({x1, y1}, {x2, y2}, {r, g, b})
int LuaAPI::UI_DrawLine(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_uiSystem || !lua.isTable(1) || !lua.isTable(2) || !lua.isTable(3)) return 0;

    // lua.rawGetI always goes: (table, index)

    // x1, y1
    lua.rawGetI(1, 1);
    uint16_t x1 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(1, 2);
    uint16_t y1 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    // x2, y2
    lua.rawGetI(2, 1);
    uint16_t x2 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(2, 2);
    uint16_t y2 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    // r, g, b
    lua.rawGetI(3, 1);
    uint8_t r = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(3, 2);
    uint8_t g = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(3, 3);
    uint8_t b = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    /*auto& frag = balloc.allocateFragment<psyqo::Prim::Rectangle>();
    frag.primitive.setColor(psyqo::Color{ .r = r, .g = g, .b = b });
    frag.primitive.position = { .x = x1, .y = y1 };
    frag.primitive.size = { .x = x2, .y = y2 };
    frag.primitive.setOpaque();
    ot.insert(frag, 0);*/

    //renderLine(nullptr, nullptr);

    auto& gpu = Renderer::GetInstance().getGPU();

    psyqo::Prim::GouraudLine thisLine;

    thisLine.pointA.x = x1;
    thisLine.pointA.y = y1;

    thisLine.pointB.x = x2;
    thisLine.pointB.y = y2;

    thisLine.setColorA(psyqo::Color{ r, g, b });
    thisLine.setColorB(psyqo::Color{ r, g, b });

    gpu.sendPrimitive(thisLine);

    return 0;
}

// parameters: UI_DrawTriangle({x1, y1}, {x2, y2}, {x3, y3}, {r1, g1, b1}, {r2, g2, b2}, {r3, g3, b3})
int LuaAPI::UI_DrawTriangle(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_uiSystem || !lua.isTable(1) || !lua.isTable(2) || !lua.isTable(3)) return 0;

    // lua.rawGetI always goes: (table, index)

    // x1, y1
    lua.rawGetI(1, 1);
    uint16_t x1 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(1, 2);
    uint16_t y1 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    // x2, y2
    lua.rawGetI(2, 1);
    uint16_t x2 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(2, 2);
    uint16_t y2 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    // x3, y3
    lua.rawGetI(3, 1);
    uint16_t x3 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(3, 2);
    uint16_t y3 = (uint16_t)lua.checkNumber(-1);
    lua.pop();

    // r1, g1, b1
    lua.rawGetI(4, 1);
    uint8_t r1 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(4, 2);
    uint8_t g1 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(4, 3);
    uint8_t b1 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    // r2, g2, b2
    lua.rawGetI(5, 1);
    uint8_t r2 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(5, 2);
    uint8_t g2 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(5, 3);
    uint8_t b2 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    // r3, g3, b3
    lua.rawGetI(6, 1);
    uint8_t r3 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(6, 2);
    uint8_t g3 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    lua.rawGetI(6, 3);
    uint8_t b3 = (uint8_t)lua.checkNumber(-1);
    lua.pop();

    /*auto& frag = balloc.allocateFragment<psyqo::Prim::Rectangle>();
    frag.primitive.setColor(psyqo::Color{ .r = r, .g = g, .b = b });
    frag.primitive.position = { .x = x1, .y = y1 };
    frag.primitive.size = { .x = x2, .y = y2 };
    frag.primitive.setOpaque();
    ot.insert(frag, 0);*/

    //renderLine(nullptr, nullptr);

    auto& gpu = Renderer::GetInstance().getGPU();

    psyqo::Prim::GouraudTriangle thisTri;

    thisTri.pointA.x = x1;
    thisTri.pointA.y = y1;

    thisTri.pointB.x = x2;
    thisTri.pointB.y = y2;

    thisTri.pointC.x = x3;
    thisTri.pointC.y = y3;

    thisTri.setColorA(psyqo::Color{ r1, g1, b1 });
    thisTri.setColorB(psyqo::Color{ r2, g2, b2 });
    thisTri.setColorC(psyqo::Color{ r3, g3, b3 });

    gpu.sendPrimitive(thisTri);

    return 0;
}

// ============================================================================
// PLAYER API IMPLEMENTATION
// ============================================================================

int LuaAPI::Player_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager) return 0;
    
    // vec3
    if(lua.isTable(1)){
        psyqo::FixedPoint<12> x, y, z;
        ReadVec3(lua, 1, x, y, z);
        s_sceneManager->setPlayerPosition(x,y,z);
        return 0;
    }
    
    // Three numbers passed in world coordinates 
    if(lua.isNumber(1) && lua.isNumber(2) && lua.isNumber(3)){
        psyqo::FixedPoint<12> x, y, z;
        x = psyqo::FixedPoint<12>(static_cast<int32_t>(lua.toNumber(1)), psyqo::FixedPoint<12>::RAW);
        y = psyqo::FixedPoint<12>(static_cast<int32_t>(lua.toNumber(2)), psyqo::FixedPoint<12>::RAW);
        z = psyqo::FixedPoint<12>(static_cast<int32_t>(lua.toNumber(3)), psyqo::FixedPoint<12>::RAW);

        s_sceneManager->setPlayerPosition(x,y,z);
    }

    return 0;
}

int LuaAPI::Player_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_sceneManager) {
        psyqo::Vec3 pos = s_sceneManager->getPlayerPosition();
        PushVec3(lua, pos.x, pos.y, pos.z);
    } else {
        PushVec3(lua, psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0));
    }
    return 1;
}

int LuaAPI::Player_SetRotation(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager || !lua.isTable(1)) return 0;
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);

    s_sceneManager->setPlayerRotation(x,y,z);
    
    return 0;
}

int LuaAPI::Player_GetRotation(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_sceneManager) {
        psyqo::Vec3 pos = s_sceneManager->getPlayerRotation();
        PushVec3(lua, pos.x, pos.y, pos.z);
    } else {
        PushVec3(lua, psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0));
    }
    return 1;
}

// ============================================================================
// SPRITE API
// ============================================================================
//
// Handles are ints into a fixed pool, not userdata: they survive being stashed
// in a Lua table across a scene tick and cost nothing to pass around.
//
// Sheets and animations are authored into the splashpack, so a script names them
// and gets an index. A name that does not exist returns -1 rather than raising -
// a missing sheet should show up as a missing sprite, not a dead scene script.

int LuaAPI::Sprite_SheetIndex(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem || !lua.isString(1)) {
        lua.pushNumber(-1);
        return 1;
    }
    lua.pushNumber(s_spriteSystem->sheetIndex(lua.toString(1)));
    return 1;
}

int LuaAPI::Sprite_AnimIndex(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem || !lua.isString(1)) {
        lua.pushNumber(-1);
        return 1;
    }
    lua.pushNumber(s_spriteSystem->animIndex(lua.toString(1)));
    return 1;
}

int LuaAPI::Sprite_Create(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) {
        lua.pushNumber(-1);
        return 1;
    }
    // Accept a sheet name or an index, so callers can skip the SheetIndex step.
    int sheet = lua.isString(1) ? s_spriteSystem->sheetIndex(lua.toString(1))
                                : static_cast<int>(lua.checkNumber(1));
    lua.pushNumber(s_spriteSystem->create(sheet));
    return 1;
}

int LuaAPI::Sprite_Destroy(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->destroy(static_cast<int>(lua.checkNumber(1)));
    return 0;
}

int LuaAPI::Sprite_SetPos(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setPos(static_cast<int>(lua.checkNumber(1)),
                           static_cast<int16_t>(lua.checkNumber(2)),
                           static_cast<int16_t>(lua.checkNumber(3)));
    return 0;
}

int LuaAPI::Sprite_SetWorldPos(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    psyqo::Vec3 p;
    p.x = readFP(lua, 2);
    p.y = readFP(lua, 3);
    p.z = readFP(lua, 4);
    s_spriteSystem->setWorldPos(static_cast<int>(lua.checkNumber(1)), p);
    return 0;
}

int LuaAPI::Sprite_BindToActor(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    int id = static_cast<int>(lua.checkNumber(1));

    GameObject* go = nullptr;
    if (lua.isTable(2)) {
        // An entity handle carries __cpp_ptr; an Actor.Find handle carries only
        // __actor_id. Accept either - sprites are almost always bound to actors,
        // and reading __cpp_ptr off an actor table would (silently) bind to null,
        // leaving every sprite stacked at the origin and dragged only by the view
        // offset.
        lua.getField(2, "__cpp_ptr");
        go = lua.toUserdata<GameObject>(-1);
        lua.pop();
        if (!go && s_sceneManager) {
            go = s_sceneManager->getActorGameObject(ReadActorId(lua, 2));
        }
    }

    // Default to the screen plane: world-space sprites are carried through the
    // data model but not drawn yet.
    SpritePlane plane = SpritePlane::Screen;
    if (lua.isNumber(3) && static_cast<int>(lua.toNumber(3)) == 1) plane = SpritePlane::World;

    int16_t offX = lua.isNumber(4) ? static_cast<int16_t>(lua.toNumber(4)) : 0;
    int16_t offY = lua.isNumber(5) ? static_cast<int16_t>(lua.toNumber(5)) : 0;

    s_spriteSystem->bindToActor(id, go, plane, offX, offY);
    return 0;
}

int LuaAPI::Sprite_SetFrame(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setFrame(static_cast<int>(lua.checkNumber(1)),
                             static_cast<uint8_t>(lua.checkNumber(2)));
    return 0;
}

int LuaAPI::Sprite_PlayAnim(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    int id = static_cast<int>(lua.checkNumber(1));
    int anim = lua.isString(2) ? s_spriteSystem->animIndex(lua.toString(2))
                               : static_cast<int>(lua.checkNumber(2));
    // Default to NOT restarting: re-issuing the same anim every frame (the
    // natural way to write a movement script) must not pin it to frame 0.
    bool restart = lua.isBoolean(3) ? lua.toBoolean(3) : false;
    s_spriteSystem->playAnim(id, anim, restart);
    return 0;
}

int LuaAPI::Sprite_StopAnim(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->stopAnim(static_cast<int>(lua.checkNumber(1)));
    return 0;
}

int LuaAPI::Sprite_SetFacingFromYaw(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    int id = static_cast<int>(lua.checkNumber(1));
    int animBase = lua.isString(2) ? s_spriteSystem->animIndex(lua.toString(2))
                                   : static_cast<int>(lua.checkNumber(2));
    uint8_t dirCount = static_cast<uint8_t>(lua.checkNumber(3));

    // Yaw arrives in the same units as Entity.SetRotationY: a Lua number where
    // 1.0 is 180 degrees. Convert to the raw Angle the math expects.
    psyqo::FixedPoint<12> fp12 = readFP(lua, 4);
    int32_t yawRaw = fp12.value >> 2;

    s_spriteSystem->setFacingFromYaw(id, animBase, dirCount, yawRaw);
    return 0;
}

int LuaAPI::Sprite_SetVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setVisible(static_cast<int>(lua.checkNumber(1)), lua.toBoolean(2));
    return 0;
}

int LuaAPI::Sprite_IsVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) {
        lua.push(false);
        return 1;
    }
    lua.push(s_spriteSystem->isVisible(static_cast<int>(lua.checkNumber(1))));
    return 1;
}

int LuaAPI::Sprite_SetFlip(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setFlip(static_cast<int>(lua.checkNumber(1)), lua.toBoolean(2),
                            lua.toBoolean(3));
    return 0;
}

int LuaAPI::Sprite_SetColor(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setColor(static_cast<int>(lua.checkNumber(1)),
                             static_cast<uint8_t>(lua.checkNumber(2)),
                             static_cast<uint8_t>(lua.checkNumber(3)),
                             static_cast<uint8_t>(lua.checkNumber(4)));
    return 0;
}

int LuaAPI::Sprite_SetLayer(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setLayer(static_cast<int>(lua.checkNumber(1)),
                             static_cast<uint8_t>(lua.checkNumber(2)));
    return 0;
}

int LuaAPI::Sprite_SetSize(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    // 0 means "the sheet's cell size", which is also the only size that takes
    // the cheap 1:1 blit instead of a quad.
    s_spriteSystem->setSize(static_cast<int>(lua.checkNumber(1)),
                            static_cast<int16_t>(lua.checkNumber(2)),
                            static_cast<int16_t>(lua.checkNumber(3)));
    return 0;
}

int LuaAPI::Sprite_SetIgnoreViewOffset(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setIgnoreViewOffset(static_cast<int>(lua.checkNumber(1)), lua.toBoolean(2));
    return 0;
}

int LuaAPI::Sprite_SetViewOffset(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_spriteSystem) return 0;
    s_spriteSystem->setViewOffset(static_cast<int16_t>(lua.checkNumber(1)),
                                  static_cast<int16_t>(lua.checkNumber(2)));
    return 0;
}

int LuaAPI::Sprite_GetViewOffset(lua_State* L) {
    psyqo::Lua lua(L);
    int16_t x = 0, y = 0;
    if (s_spriteSystem) s_spriteSystem->getViewOffset(x, y);
    lua.pushNumber(static_cast<lua_Number>(x));
    lua.pushNumber(static_cast<lua_Number>(y));
    return 2;
}

int LuaAPI::Sprite_Count(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(s_spriteSystem ? s_spriteSystem->spriteCount() : 0);
    return 1;
}

// ============================================================================
// TILE
// ============================================================================

int LuaAPI::Tile_Walkable(lua_State* L) {
    psyqo::Lua lua(L);
    // No map means no walls: a scene without a tilemap keeps free movement, so
    // the collision check callers make is a no-op there rather than a wall
    // everywhere. Off the edge of a map that DOES exist is not walkable.
    if (!s_tileSystem || !s_tileSystem->active()) {
        lua.push(true);
        return 1;
    }
    const int32_t px = static_cast<int32_t>(lua.checkNumber(1));
    const int32_t pz = static_cast<int32_t>(lua.checkNumber(2));
    lua.push(s_tileSystem->walkableAtPixel(px, pz));
    return 1;
}

// Tile.RayClear(x0, z0, x1, z1) -> bool
//
// Is there an unobstructed straight line between two world pixels? This is the
// primitive a game needs for line-of-sight, and it is native for the same reason
// collision is: doing it in Lua costs one Tile.Walkable call per sample, which is
// hundreds of Lua-to-C++ transitions per frame on a 33MHz CPU.
//
// A scene with no tilemap reports true - nothing exists to block a view - which
// matches how Tile.Walkable and Tile.MoveActor degrade without a map.
int LuaAPI::Tile_RayClear(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_tileSystem || !s_tileSystem->active()) {
        lua.push(true);
        return 1;
    }
    const int32_t x0 = static_cast<int32_t>(lua.checkNumber(1));
    const int32_t z0 = static_cast<int32_t>(lua.checkNumber(2));
    const int32_t x1 = static_cast<int32_t>(lua.checkNumber(3));
    const int32_t z1 = static_cast<int32_t>(lua.checkNumber(4));
    lua.push(s_tileSystem->sightClear(x0, z0, x1, z1));
    return 1;
}

// Tile.MoveActor(actor, dx, dz) -> newX, newZ
//
// Move an actor by (dx, dz) pixels on the tile ground plane, clamped against the
// map's walls in NATIVE code - the collision resolution belongs here, not in a
// per-frame Lua walkability dance. Each axis is tested on its own so a wall stops
// that direction while the actor keeps sliding along it, and X is resolved before
// Z so corners read cleanly. With no tilemap the full move is applied, so a caller
// can always route movement through this and get free movement where there is no
// map and wall collision where there is. Returns the resulting pixel position.
int LuaAPI::Tile_MoveActor(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) {
        lua.push(nullptr);
        return 1;
    }

    const uint16_t actorId = ReadActorId(lua, 1);
    psyqo::Vec3 pos;
    if (!s_sceneManager->getActorPosition(actorId, pos)) {
        lua.push(nullptr);
        return 1;
    }

    const int32_t dx = static_cast<int32_t>(lua.checkNumber(2));
    const int32_t dz = static_cast<int32_t>(lua.checkNumber(3));

    // Tile.MoveActor(actor, dx, dz [, ignoreWalls])
    //
    // The optional fourth argument moves the actor without consulting the
    // tilemap at all. It exists because "solid to the world" is a property of
    // the MOVER, not of the map: a ghost, a spectator camera or anything else
    // that is present but not physical still wants the rest of this function -
    // the actor lookup, the integer-pixel semantics, the untouched y - and
    // reimplementing that in Lua to get around the collision check would mean
    // two movement paths that have to be kept in step.
    const bool ignoreWalls = lua.isBoolean(4) && lua.toBoolean(4);

    const int32_t px = pos.x.integer();
    const int32_t pz = pos.z.integer();
    int32_t nx = px, nz = pz;

    if (!ignoreWalls && s_tileSystem && s_tileSystem->active()) {
        if (dx != 0 && s_tileSystem->walkableAtPixel(px + dx, pz)) nx = px + dx;
        if (dz != 0 && s_tileSystem->walkableAtPixel(nx, pz + dz)) nz = pz + dz;
    } else {
        nx = px + dx;
        nz = pz + dz;
    }

    if (nx != px || nz != pz) {
        // Keep y untouched (a jumping/elevated actor must not be snapped down).
        pos.x = psyqo::FixedPoint<12>(nx * 4096, psyqo::FixedPoint<12>::RAW);
        pos.z = psyqo::FixedPoint<12>(nz * 4096, psyqo::FixedPoint<12>::RAW);
        s_sceneManager->setActorPosition(actorId, pos);
    }

    lua.pushNumber(nx);
    lua.pushNumber(nz);
    return 2;
}

int LuaAPI::Tile_Active(lua_State* L) {
    psyqo::Lua lua(L);
    lua.push(s_tileSystem && s_tileSystem->active());
    return 1;
}

int LuaAPI::Tile_MapSize(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_tileSystem || !s_tileSystem->active()) {
        lua.pushNumber(0);
        lua.pushNumber(0);
        lua.pushNumber(0);
        lua.pushNumber(0);
        return 4;
    }
    lua.pushNumber(s_tileSystem->width());
    lua.pushNumber(s_tileSystem->height());
    lua.pushNumber(s_tileSystem->tileW());
    lua.pushNumber(s_tileSystem->tileH());
    return 4;
}

int LuaAPI::Tile_ObjectCount(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(s_tileSystem ? s_tileSystem->objectCount() : 0);
    return 1;
}

// Tile.ObjectAt(i) -> kind, id, x, z  (1-based i; nil if out of range). The x/z
// are the object tile's CENTRE in world pixels, the natural point to stand on or
// to measure distance to.
int LuaAPI::Tile_ObjectAt(lua_State* L) {
    psyqo::Lua lua(L);
    const int idx = static_cast<int>(lua.checkNumber(1)) - 1;  // Lua is 1-based
    const TileObject* o = s_tileSystem ? s_tileSystem->object(idx) : nullptr;
    if (!o) {
        lua.push(nullptr);
        return 1;
    }
    int32_t px = 0, pz = 0;
    s_tileSystem->objectCenterPixel(idx, &px, &pz);
    lua.pushNumber(o->kind);
    lua.pushNumber(o->id);
    lua.pushNumber(px);
    lua.pushNumber(pz);
    return 4;
}

}  // namespace psxsplash
