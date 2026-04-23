TARGET = psxsplash
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/renderer.cpp \
src/splashpack.cpp \
src/camera.cpp \
src/gtemath.cpp \
src/worldcollision.cpp \
src/navregion.cpp \
src/random.cpp\
src/lua.cpp \
src/luaapi.cpp \
src/scenemanager.cpp \
src/fileloader.cpp \
src/audiomanager.cpp \
src/controls.cpp \
src/profiler.cpp \
src/collision.cpp \
src/bvh.cpp \
src/cutscene.cpp \
src/interpolation.cpp \
src/animation.cpp \
src/uisystem.cpp \
src/loadingscreen.cpp \
src/memoverlay.cpp \
src/musicmanager.cpp \
src/skinmesh.cpp \
src/loadbuffer_patch.cpp

# LOADER=cdrom  → CD-ROM backend (for ISO builds on real hardware)
# LOADER=pcdrv  → PCdrv backend (default, emulator + SIO1)
ifeq ($(LOADER),cdrom)
CPPFLAGS += -DLOADER_CDROM
else
CPPFLAGS += -DPCDRV_SUPPORT=1
endif

# MEMOVERLAY=1  → Enable runtime heap/RAM usage overlay
ifeq ($(MEMOVERLAY),1)
CPPFLAGS += -DPSXSPLASH_MEMOVERLAY
endif

# FPSOVERLAY=1  → Enable runtime FPS overlay
ifeq ($(FPSOVERLAY), 1)
CPPFLAGS += -DPSXSPLASH_FPSOVERLAY
endif

# ROOMDEBUG=1  → Enable room topology debug overlay
ifeq ($(ROOMDEBUG),1)
CPPFLAGS += -DPSXSPLASH_ROOM_DEBUG
endif

# PROFILER=1  → Enable per-frame profiler overlay + PCSX variable export
ifeq ($(PROFILER),1)
CPPFLAGS += -DPSXSPLASH_PROFILER
endif

ifdef OT_SIZE
CPPFLAGS += -DOT_SIZE=$(OT_SIZE)
endif
ifdef BUMP_SIZE
CPPFLAGS += -DBUMP_SIZE=$(BUMP_SIZE)
endif

include third_party/nugget/psyqo-lua/psyqo-lua.mk
include third_party/nugget/psyqo/psyqo.mk

# Redirect Lua's allocator through our OOM-guarded wrapper
LDFLAGS := $(subst psyqo_realloc,lua_oom_realloc,$(LDFLAGS))

# NOPARSER=1  → Use precompiled bytecode, strip Lua parser from runtime (~25KB savings)
ifeq ($(NOPARSER),1)
LIBRARIES := $(subst liblua.a,liblua-noparser.a,$(LIBRARIES))
# Wrap luaL_loadbufferx to intercept psyqo-lua's source-text FixedPoint
# metatable init and redirect it to pre-compiled bytecode.
LDFLAGS += -Wl,--wrap=luaL_loadbufferx
endif

