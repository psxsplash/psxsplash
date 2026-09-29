-- The Makefile builds everything with -Os; match it, psxsplash has to fit a
-- scene in 2 MB of RAM.
add_cxflags("-Os", {force = true})
add_cflags("-Os", {force = true})


includes("third_party/nugget/psyqo-lua")

option("loader", {default = "pcdrv", values = {"pcdrv", "cdrom"},
    description = "File backend: pcdrv (emulator and SIO1) or cdrom (ISO builds for real hardware)"})
option("noparser", {default = false,
    description = "Link Lua without its parser; scripts must be precompiled bytecode"})
option("memoverlay", {default = false, description = "Runtime heap/RAM usage overlay"})
option("fpsoverlay", {default = false, description = "Runtime FPS overlay"})
option("roomdebug", {default = false, description = "Room topology debug overlay"})
option("profiler", {default = false, description = "Per-frame profiler overlay and PCSX variable export"})
option("ot_size", {default = nil, type = "string", description = "Ordering table size"})
option("bump_size", {default = nil, type = "string", description = "Bump allocator size"})

target("psxsplash", function()
    add_rules("psyqo-lua.app")
    add_files("src/*.cpp")

    if is_config("loader", "cdrom") then
        add_defines("LOADER_CDROM")
    else
        add_defines("PCDRV_SUPPORT=1")
    end
    if has_config("memoverlay") then add_defines("PSXSPLASH_MEMOVERLAY") end
    if has_config("fpsoverlay") then add_defines("PSXSPLASH_FPSOVERLAY") end
    if has_config("roomdebug") then add_defines("PSXSPLASH_ROOM_DEBUG") end
    if has_config("profiler") then add_defines("PSXSPLASH_PROFILER") end
    if has_config("ot_size") then add_defines("OT_SIZE=" .. get_config("ot_size")) end
    if has_config("bump_size") then add_defines("BUMP_SIZE=" .. get_config("bump_size")) end
    if has_config("noparser") then
        -- Wrap luaL_loadbufferx so psyqo-lua's FixedPoint metatable source
        -- is replaced by precompiled bytecode.
        add_values("psyqo-lua.noparser", true)
        add_ldflags("-Wl,--wrap=luaL_loadbufferx", {force = true})
    end

    -- Send Lua's allocator through the OOM-logging wrapper in src/lua.cpp.
    -- This has to come after psyqo-lua's own luaI_realloc definition.
    add_ldflags("-Wl,--defsym,luaI_realloc=lua_oom_realloc", {force = true})
end)

target("psxsplash.ps-exe", function()
    add_deps("psxsplash")
    add_rules("ps-exe")
end)
