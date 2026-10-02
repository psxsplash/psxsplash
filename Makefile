TARGET = psxsplash
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/renderer.cpp \
src/splashpack.cpp \
src/camera.cpp \
src/worldcollision.cpp \
src/navregion.cpp \
src/random.cpp\
src/luautility.cpp\
src/lua.cpp \
src/luaapi.cpp \
src/scenemanager.cpp \
src/fileloader.cpp \
src/audiomanager.cpp \
src/controls.cpp \
src/profiler.cpp \
src/bvh.cpp \
src/memoverlay.cpp \
src/musicmanager.cpp \
src/loadbuffer_patch.cpp

# FEATURES     → Engine subsystems to compile in (see src/features.hh), space or
#                comma separated. Unset builds all of them. SplashEdit passes
#                the set the exported scenes use, e.g. FEATURES=ui,collision,nav.
#                FEATURES= or FEATURES=none builds the smallest engine.
ALL_FEATURES = net ui sprites skin cutscene lights streaming memcard nav agents collision
ifeq ($(origin FEATURES),undefined)
FEATURES := $(ALL_FEATURES)
endif
comma := ,
override FEATURES := $(filter-out none,$(subst $(comma), ,$(FEATURES)))
ifneq ($(filter-out $(ALL_FEATURES),$(FEATURES)),)
$(error Unknown FEATURES: $(filter-out $(ALL_FEATURES),$(FEATURES)). Known: $(ALL_FEATURES))
endif
# Agents path along nav regions.
ifneq ($(filter agents,$(FEATURES)),)
override FEATURES += nav
endif

FEATURE_SRCS_net = src/sio1.cpp src/netlink.cpp src/nettest.cpp src/networkmanager.cpp src/luatableserializer.cpp
FEATURE_SRCS_ui = src/uisystem.cpp src/loadingscreen.cpp
FEATURE_SRCS_sprites = src/spritesystem.cpp src/spritemath.cpp src/tilesystem.cpp src/tilemath.cpp
FEATURE_SRCS_skin = src/skinmesh.cpp
FEATURE_SRCS_cutscene = src/cutscene.cpp src/interpolation.cpp src/animation.cpp
FEATURE_SRCS_lights = src/lightmath.cpp
FEATURE_SRCS_streaming = src/streamreader.cpp src/streamselftest.cpp src/worldstreamer.cpp src/streamplanner.cpp
FEATURE_SRCS_memcard = src/memorycardmanager.cpp src/luatableserializer.cpp
FEATURE_SRCS_collision = src/collision.cpp

SRCS += $(sort $(foreach f,$(FEATURES),$(FEATURE_SRCS_$(f))))

feature_uc = $(subst a,A,$(subst b,B,$(subst c,C,$(subst d,D,$(subst e,E,$(subst g,G,$(subst h,H,$(subst i,I,$(subst k,K,$(subst l,L,$(subst m,M,$(subst n,N,$(subst o,O,$(subst p,P,$(subst r,R,$(subst s,S,$(subst t,T,$(subst u,U,$(subst v,V,$(subst w,W,$(1)))))))))))))))))))))
CPPFLAGS += $(foreach f,$(ALL_FEATURES),-DPSXSPLASH_FEATURE_$(call feature_uc,$(f))=$(if $(filter $(f),$(FEATURES)),1,0))

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

# SIO1ECHO=1  → Build the raw SIO1 link self-test in place of the game loop.
#               Run two instances linked over SIO1 (emulator: one as the SIO1
#               TCP server, the other as client) and watch RX/TX counters climb.
ifeq ($(SIO1ECHO),1)
ifeq ($(filter net,$(FEATURES)),)
$(error SIO1ECHO=1 needs the net feature)
endif
CPPFLAGS += -DPSXSPLASH_SIO1_ECHO
endif

# NETTEST=1   → Build the in-RAM NetLink protocol self-test in place of the game
#               loop. Runs on a single instance (no link needed); shows PASS/FAIL.
ifeq ($(NETTEST),1)
ifeq ($(filter net,$(FEATURES)),)
$(error NETTEST=1 needs the net feature)
endif
CPPFLAGS += -DPSXSPLASH_NETTEST
endif

# STREAMTEST=1 → Stream the scene's splashpack back during gameplay forever and
#                checksum every chunk (gameplay CD read stress test).
ifeq ($(STREAMTEST),1)
ifeq ($(filter streaming,$(FEATURES)),)
$(error STREAMTEST=1 needs the streaming feature)
endif
CPPFLAGS += -DPSXSPLASH_STREAM_SELFTEST
endif

# STREAMLOG=1  → Print streamed-world region attach/detach events.
ifeq ($(STREAMLOG),1)
CPPFLAGS += -DPSXSPLASH_STREAM_LOG
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
LDFLAGS := $(subst luaI_realloc=libc_realloc,luaI_realloc=lua_oom_realloc,$(LDFLAGS))

# NOPARSER=1  → Use precompiled bytecode, strip Lua parser from runtime (~25KB savings)
ifeq ($(NOPARSER),1)
LIBRARIES := $(subst liblua.a,liblua-noparser.a,$(LIBRARIES))
# Wrap luaL_loadbufferx to intercept psyqo-lua's source-text FixedPoint
# metatable init and redirect it to pre-compiled bytecode.
LDFLAGS += -Wl,--wrap=luaL_loadbufferx
endif

# make does not see a changed -D, so a build with different FEATURES (or any
# option above) would relink objects compiled for the old set. Every object
# depends on a stamp holding the flags, rewritten only when they change.
BUILD_FLAGS_STAMP = .build-flags
BUILD_FLAGS := $(strip $(CPPFLAGS))
-include $(BUILD_FLAGS_STAMP)
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifneq ($(BUILT_WITH_FLAGS),$(BUILD_FLAGS))
$(file >$(BUILD_FLAGS_STAMP),BUILT_WITH_FLAGS := $(BUILD_FLAGS))
endif
endif
$(OBJS): $(BUILD_FLAGS_STAMP)
$(BUILD_FLAGS_STAMP):
	$(file >$@,BUILT_WITH_FLAGS := $(BUILD_FLAGS))

ALL_FEATURE_OBJS = $(addsuffix .o,$(basename $(foreach f,$(ALL_FEATURES),$(FEATURE_SRCS_$(f)))))
clean::
	rm -f $(ALL_FEATURE_OBJS) $(patsubst %.o,%.dep,$(ALL_FEATURE_OBJS)) $(BUILD_FLAGS_STAMP)
