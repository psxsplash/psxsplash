# luac_psx - PS1 Lua bytecode compiler
#
# Minimal PS1 executable that compiles Lua source to bytecode.
# Links only psyqo (allocator, xprintf) and psxlua (full parser).
# No psxsplash code, no renderer, no GPU.

TARGET = luac_psx
TYPE = ps-exe

SRCS = main.c

# Relative path to nugget root
NUGGETDIR = ../../third_party/nugget

# Lua library (full parser variant - NOT noparser)
LUADIR = $(NUGGETDIR)/third_party/psxlua/src
LIBRARIES += $(LUADIR)/liblua.a

# Include paths
CPPFLAGS += -I$(LUADIR)
CPPFLAGS += -I$(NUGGETDIR)
CPPFLAGS += -DLUA_TARGET_PSX

# psyqo build system (provides libpsyqo.a, linker scripts, toolchain config)
include $(NUGGETDIR)/psyqo/psyqo.mk

# Build liblua.a if not already built
$(LUADIR)/liblua.a:
	$(MAKE) -C $(NUGGETDIR)/third_party/psxlua psx

clean::
	$(MAKE) -C $(NUGGETDIR)/third_party/psxlua clean

.PHONY: $(LUADIR)/liblua.a
