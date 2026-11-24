-- Script of every copy in the "crates" spawn pool of fixture-spawnpool (built by
-- tests/boot/tools/add_spawn_pool.py). It counts its events in the shared
-- SPAWNPOOL table so spawnpool_check.lua can tell what fired and when: onCreate
-- once per copy at load (a pool copy starts despawned but is still created),
-- onEnable on Scene.Spawn and onDisable on Scene.Despawn, never at load.
_G.SPAWNPOOL = _G.SPAWNPOOL or { creates = 0, enables = 0, disables = 0 }

function onCreate(self)
    _G.SPAWNPOOL.creates = _G.SPAWNPOOL.creates + 1
    Debug.Log("SPAWNPOOL copy onCreate " .. tostring(Actor.GetName(self)) .. " active " .. tostring(Entity.IsActive(self)))
end

function onEnable(self)
    _G.SPAWNPOOL.enables = _G.SPAWNPOOL.enables + 1
end

function onDisable(self)
    _G.SPAWNPOOL.disables = _G.SPAWNPOOL.disables + 1
end
