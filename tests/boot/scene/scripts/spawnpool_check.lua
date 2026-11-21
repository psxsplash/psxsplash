-- Scene script of fixture-spawnpool: the "crates" spawn pool of 4 copies of the
-- Crate (tests/boot/tools/add_spawn_pool.py). Prints SPAWNPOOL OK <spawned>
-- <capacity> only when every expectation held, else SPAWNPOOL FAIL <reason>.
local CAP = 4
local one = FixedPoint.new(1)   -- the only way to write a fraction
local half = one / 2

local function fail(why)
    Debug.Log("SPAWNPOOL FAIL " .. why)
end

local function counts(label)
    local a, c = Scene.PoolCount("crates")
    Debug.Log("SPAWNPOOL " .. label .. " count " .. a .. "/" .. c)
    return a, c
end

function onSceneCreationEnd()
    local S = _G.SPAWNPOOL
    if not S then return fail("copy script never ran") end

    -- At load: every copy despawned, yet each was created once and none enabled.
    local a, c = counts("load")
    if a ~= 0 or c ~= CAP then return fail("load count " .. a .. "/" .. c) end
    if S.creates ~= CAP then return fail("onCreate ran " .. S.creates .. " times at load") end
    if S.enables ~= 0 or S.disables ~= 0 then return fail("enable/disable events at load") end

    -- Unknown pool, and Despawn of what is no pool copy: nothing happens, nothing errors.
    local ua, uc = Scene.PoolCount("nope")
    if ua ~= 0 or uc ~= 0 then return fail("unknown pool count " .. ua .. "/" .. uc) end
    if Scene.Spawn("nope", 0, 0, 0) ~= nil then return fail("spawn of an unknown pool") end
    local crate = Entity.Find("Crate")
    if not crate then return fail("no Crate") end
    if Scene.Despawn(crate) ~= false then return fail("despawn of a non-pool object") end
    if not Entity.IsActive(crate) then return fail("despawn of a non-pool object deactivated it") end
    if Scene.Despawn(nil) ~= false then return fail("despawn of nil") end
    if Scene.Despawn({}) ~= false then return fail("despawn of a plain table") end

    -- What Entity.SetPosition / SetRotationY do to the Crate is what Spawn must do to a copy.
    Entity.SetPosition(crate, Vec3.new(3, 1, 4))
    Entity.SetRotationY(crate, half)
    local wantPos, wantRot = Entity.GetPosition(crate), Entity.GetRotationY(crate)

    -- Spawn until nil: lowest free copy first, each activated, each reported.
    local spawned = {}
    while #spawned < 16 do
        local h
        if #spawned == 0 then
            h = Scene.Spawn("crates", 3, 1, 4, half)
        else
            h = Scene.Spawn("crates", #spawned, 0, 4)
        end
        if h == nil then break end
        spawned[#spawned + 1] = h
        local name = Actor.GetName(h)
        local ca = counts("spawned " .. tostring(name))
        if name ~= "Crate_" .. #spawned then return fail("spawn " .. #spawned .. " gave " .. tostring(name)) end
        if not Entity.IsActive(h) then return fail(name .. " not active after spawn") end
        if ca ~= #spawned then return fail("active count " .. ca .. " after " .. #spawned .. " spawns") end
        if S.enables ~= #spawned then return fail("onEnable ran " .. S.enables .. " times after " .. #spawned) end
    end
    if #spawned ~= CAP then return fail("spawned " .. #spawned .. " not " .. CAP) end

    local p = Entity.GetPosition(spawned[1])
    if not (p.x == wantPos.x and p.y == wantPos.y and p.z == wantPos.z) then return fail("spawn position differs from SetPosition") end
    if Entity.GetRotationY(spawned[1]) ~= wantRot then return fail("spawn rotation differs from SetRotationY") end

    -- Exhausted: nil again, and the pool is full.
    if Scene.Spawn("crates", 0, 0, 0) ~= nil then return fail("spawn from an exhausted pool") end
    local fa, fc = counts("full")
    if fa ~= CAP or fc ~= CAP then return fail("full count " .. fa .. "/" .. fc) end

    -- Despawn the second copy: true once, false after; the slot is free and handed out again.
    if Scene.Despawn(spawned[2]) ~= true then return fail("despawn of an active copy") end
    if S.disables ~= 1 then return fail("onDisable ran " .. S.disables .. " times") end
    if Entity.IsActive(spawned[2]) then return fail("copy still active after despawn") end
    if Scene.Despawn(spawned[2]) ~= false then return fail("second despawn of the same copy") end
    if S.disables ~= 1 then return fail("second despawn fired onDisable") end
    local da = counts("despawned")
    if da ~= CAP - 1 then return fail("count after despawn " .. da) end
    local again = Scene.Spawn("crates", 9, 9, 9)
    if again == nil then return fail("no copy after a despawn") end
    local name = Actor.GetName(again)
    Debug.Log("SPAWNPOOL respawned " .. tostring(name))
    if name ~= "Crate_2" then return fail("respawn gave " .. tostring(name)) end
    if S.enables ~= CAP + 1 then return fail("onEnable ran " .. S.enables .. " times in all") end
    if Scene.Spawn("crates", 0, 0, 0) ~= nil then return fail("spawn from a pool refilled") end

    Debug.Log("SPAWNPOOL OK " .. #spawned .. " " .. CAP)
end
