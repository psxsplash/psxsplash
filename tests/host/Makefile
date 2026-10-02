# Host test harness for the networking stack.
#
# Compiles the REAL engine sources (netlink.cpp and the pure protocol headers)
# with the host compiler against a mock transport — no MIPS toolchain, no
# emulator, no PlayStation. See README.md for the MSVC path on Windows.
#
#   make          build + run the unit tests
#   make vectors  regenerate the cross-language frame corpus
#   make clean

CXX      ?= g++
SRC_DIR  := ../../src
VEC_DIR  := ../vectors
BUILD    := build

# -I. puts compat.hh and the test headers ahead of the engine sources.
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I. -I$(SRC_DIR)

# The engine sources with logic worth compiling here; everything else the tests
# touch is header-only (netprotocol.hh, ringbuffer.hh, inettransport.hh).
# spritemath.cpp is split out of spritesystem.cpp precisely so it carries no
# psyqo dependency and can be built natively.
NET_SRCS    := $(SRC_DIR)/netlink.cpp
SPRITE_SRCS := $(SRC_DIR)/spritemath.cpp
TILE_SRCS   := $(SRC_DIR)/tilemath.cpp
LIGHT_SRCS  := $(SRC_DIR)/lightmath.cpp

TEST_BIN   := $(BUILD)/netlink_tests
SPRITE_BIN := $(BUILD)/sprite_tests
TILE_BIN   := $(BUILD)/tile_tests
LIGHT_BIN  := $(BUILD)/light_tests
GEN_BIN    := $(BUILD)/gen_vectors

.PHONY: all test vectors clean

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(TEST_BIN): test_netlink.cpp mocktransport.hh testing.hh compat.hh $(NET_SRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_netlink.cpp $(NET_SRCS)

$(SPRITE_BIN): test_sprite.cpp testing.hh compat.hh $(SPRITE_SRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_sprite.cpp $(SPRITE_SRCS)

$(TILE_BIN): test_tile.cpp testing.hh compat.hh $(TILE_SRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_tile.cpp $(TILE_SRCS)

$(LIGHT_BIN): test_light.cpp testing.hh compat.hh $(LIGHT_SRCS) $(SRC_DIR)/lightmath.hh | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_light.cpp $(LIGHT_SRCS)

$(GEN_BIN): gen_vectors.cpp mocktransport.hh compat.hh $(NET_SRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ gen_vectors.cpp $(NET_SRCS)

test: $(TEST_BIN) $(SPRITE_BIN) $(TILE_BIN) $(LIGHT_BIN)
	@echo "== netlink =="
	@$(TEST_BIN)
	@echo "== sprite =="
	@$(SPRITE_BIN)
	@echo "== tile =="
	@$(TILE_BIN)
	@echo "== light =="
	@$(LIGHT_BIN)

vectors: $(GEN_BIN)
	@mkdir -p $(VEC_DIR)
	@$(GEN_BIN) $(VEC_DIR)

clean:
	rm -rf $(BUILD)
