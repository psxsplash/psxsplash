#pragma once

// Engine subsystems that can be left out of the binary. The Makefile sets each
// one from FEATURES= (unset builds everything); SplashEdit passes the set the
// exported scenes use. A build that does not go through the Makefile gets all
// of them.
//
// A Lua namespace belonging to a feature that is off is still registered, as a
// table whose every field raises an error naming the feature, so a script that
// needs it fails with a message instead of calling into nothing.

#ifndef PSXSPLASH_FEATURE_NET
#define PSXSPLASH_FEATURE_NET 1        // SIO1 multiplayer: NetworkManager, Net.*
#endif
#ifndef PSXSPLASH_FEATURE_UI
#define PSXSPLASH_FEATURE_UI 1         // UI canvases, loading screens, UI.*
#endif
#ifndef PSXSPLASH_FEATURE_SPRITES
#define PSXSPLASH_FEATURE_SPRITES 1    // sprite sheets and tilemaps: Sprite.*, Tile.*
#endif
#ifndef PSXSPLASH_FEATURE_SKIN
#define PSXSPLASH_FEATURE_SKIN 1       // skinned meshes: SkinnedAnim.*
#endif
#ifndef PSXSPLASH_FEATURE_CUTSCENE
#define PSXSPLASH_FEATURE_CUTSCENE 1   // cutscenes and animations: Cutscene.*, Animation.*
#endif
#ifndef PSXSPLASH_FEATURE_LIGHTS
#define PSXSPLASH_FEATURE_LIGHTS 1     // dynamic point lights: Light.*
#endif
#ifndef PSXSPLASH_FEATURE_STREAMING
#define PSXSPLASH_FEATURE_STREAMING 1  // gameplay CD reads and streamed world regions
#endif
#ifndef PSXSPLASH_FEATURE_MEMCARD
#define PSXSPLASH_FEATURE_MEMCARD 1    // memory card saves: MemCard.*
#endif
#ifndef PSXSPLASH_FEATURE_NAV
#define PSXSPLASH_FEATURE_NAV 1        // nav regions: player walking, gravity, Actor.FindPath
#endif
#ifndef PSXSPLASH_FEATURE_AGENTS
#define PSXSPLASH_FEATURE_AGENTS 1     // native AI agents: Agent.*
#endif
#ifndef PSXSPLASH_FEATURE_COLLISION
#define PSXSPLASH_FEATURE_COLLISION 1  // object colliders and trigger boxes
#endif

#if PSXSPLASH_FEATURE_AGENTS && !PSXSPLASH_FEATURE_NAV
#error "PSXSPLASH_FEATURE_AGENTS needs PSXSPLASH_FEATURE_NAV: agents path along nav regions"
#endif
