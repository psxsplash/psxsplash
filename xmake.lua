-- The Makefile builds everything with -Os; match it, psxsplash has to fit a
-- scene in 2 MB of RAM.
add_cxflags("-Os", {force = true})
add_cflags("-Os", {force = true})

includes("third_party/nugget/psyqo-lua")

-- Engine subsystems and the sources each one adds, as in the Makefile and
-- src/features.hh.
local all_features = {"bootlogo", "net", "ui", "sprites", "skin", "cutscene", "lights", "streaming",
                      "memcard", "nav", "agents", "collision"}
local feature_srcs = {
    bootlogo = {"src/bootlogo.cpp"},
    net = {"src/sio1.cpp", "src/netlink.cpp", "src/nettest.cpp", "src/networkmanager.cpp",
           "src/luatableserializer.cpp"},
    ui = {"src/uisystem.cpp", "src/loadingscreen.cpp"},
    sprites = {"src/spritesystem.cpp", "src/spritemath.cpp", "src/tilesystem.cpp", "src/tilemath.cpp"},
    skin = {"src/skinmesh.cpp"},
    cutscene = {"src/cutscene.cpp", "src/interpolation.cpp", "src/animation.cpp"},
    lights = {"src/lightmath.cpp"},
    streaming = {"src/streamreader.cpp", "src/streamselftest.cpp", "src/worldstreamer.cpp",
                 "src/streamplanner.cpp"},
    memcard = {"src/memorycardmanager.cpp", "src/luatableserializer.cpp"},
    collision = {"src/collision.cpp"},
}

option("features", {type = "string",
    description = "Engine subsystems to compile in, comma separated (unset: all; none: the smallest engine). Known: "
        .. table.concat(all_features, ",")})
option("loader", {default = "pcdrv", values = {"pcdrv", "cdrom"},
    description = "File backend: pcdrv (emulator and SIO1) or cdrom (ISO builds for real hardware)"})
option("noparser", {default = false,
    description = "Link Lua without its parser; scripts must be precompiled bytecode"})
option("memoverlay", {default = false, description = "Runtime heap/RAM usage overlay"})
option("fpsoverlay", {default = false, description = "Runtime FPS overlay"})
option("roomdebug", {default = false, description = "Room topology debug overlay"})
option("profiler", {default = false, description = "Per-frame profiler overlay and PCSX variable export"})
option("sio1echo", {default = false, description = "Raw SIO1 link self-test in place of the game loop (needs net)"})
option("nettest", {default = false, description = "In-RAM NetLink protocol self-test in place of the game loop (needs net)"})
option("streamtest", {default = false, description = "Stream the splashpack back forever and checksum it (needs streaming)"})
option("streamlog", {default = false, description = "Print streamed-world region attach/detach events"})
option("ot_size", {type = "string", description = "Ordering table size"})
option("bump_size", {type = "string", description = "Bump allocator size"})

-- Description scope cannot raise, so bad option combinations are collected
-- here and reported when the target is configured.
local config_errors = {}

local function selected_features()
    local selected = {}
    local value = get_config("features")
    -- An unset string option reads nil before configuring and false after.
    if not value then
        for _, f in ipairs(all_features) do selected[f] = true end
    else
        for f in tostring(value):gmatch("[^%s,]+") do
            if f ~= "none" then
                if not table.contains(all_features, f) then
                    table.insert(config_errors, "unknown feature " .. f .. ", known: " .. table.concat(all_features, ","))
                end
                selected[f] = true
            end
        end
    end
    -- Agents path along nav regions.
    if selected.agents then selected.nav = true end
    return selected
end

target("psxsplash", function()
    add_rules("psyqo-lua.app")
    add_files("src/main.cpp", "src/renderer.cpp", "src/splashpack.cpp", "src/camera.cpp",
              "src/worldcollision.cpp", "src/navregion.cpp", "src/random.cpp", "src/luautility.cpp",
              "src/lua.cpp", "src/luaapi.cpp", "src/scenemanager.cpp", "src/fileloader.cpp",
              "src/audiomanager.cpp", "src/controls.cpp", "src/profiler.cpp", "src/bvh.cpp",
              "src/memoverlay.cpp", "src/musicmanager.cpp", "src/loadbuffer_patch.cpp")

    local features = selected_features()
    local added = {}
    for _, f in ipairs(all_features) do
        add_defines("PSXSPLASH_FEATURE_" .. f:upper() .. "=" .. (features[f] and "1" or "0"))
        if features[f] then
            for _, src in ipairs(feature_srcs[f] or {}) do
                if not added[src] then
                    add_files(src)
                    added[src] = true
                end
            end
        end
    end

    if is_config("loader", "cdrom") then
        add_defines("LOADER_CDROM")
    else
        add_defines("PCDRV_SUPPORT=1")
    end
    if has_config("memoverlay") then add_defines("PSXSPLASH_MEMOVERLAY") end
    if has_config("fpsoverlay") then add_defines("PSXSPLASH_FPSOVERLAY") end
    if has_config("roomdebug") then add_defines("PSXSPLASH_ROOM_DEBUG") end
    if has_config("profiler") then add_defines("PSXSPLASH_PROFILER") end
    if has_config("sio1echo") then
        if not features.net then table.insert(config_errors, "sio1echo needs the net feature") end
        add_defines("PSXSPLASH_SIO1_ECHO")
    end
    if has_config("nettest") then
        if not features.net then table.insert(config_errors, "nettest needs the net feature") end
        add_defines("PSXSPLASH_NETTEST")
    end
    if has_config("streamtest") then
        if not features.streaming then table.insert(config_errors, "streamtest needs the streaming feature") end
        add_defines("PSXSPLASH_STREAM_SELFTEST")
    end
    if has_config("streamlog") then add_defines("PSXSPLASH_STREAM_LOG") end
    if has_config("ot_size") then add_defines("OT_SIZE=" .. get_config("ot_size")) end
    if has_config("bump_size") then add_defines("BUMP_SIZE=" .. get_config("bump_size")) end
    if has_config("noparser") then
        -- Wrap luaL_loadbufferx so psyqo-lua's FixedPoint metatable source
        -- is replaced by precompiled bytecode.
        add_values("psyqo-lua.noparser", true)
        add_ldflags("-Wl,--wrap=luaL_loadbufferx", {force = true})
    end

    on_config(function(target)
        if #config_errors > 0 then
            raise(table.concat(config_errors, "; "))
        end
    end)

    -- Send Lua's allocator through the OOM-logging wrapper in src/lua.cpp.
    -- This has to come after psyqo-lua's own luaI_realloc definition.
    add_ldflags("-Wl,--defsym,luaI_realloc=lua_oom_realloc", {force = true})
end)

target("psxsplash.ps-exe", function()
    add_deps("psxsplash")
    add_rules("ps-exe")
end)
