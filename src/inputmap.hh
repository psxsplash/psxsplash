#pragma once

#include <stdint.h>

// Player numbering, device classification, button masks and mouse motion, free
// of psyqo so that tests/host/test_input.cpp compiles this exact code natively.
//
// Slots are psyqo::AdvancedPad::Pad values: 0..3 are port 1's Pad1a..Pad1d, 4..7
// port 2's Pad2a..Pad2d. Without a multitap only Pad1a and Pad2a ever connect.
// Button masks are active-high bit masks indexed by AdvancedPad::Button (the
// pad itself reports active-low).

namespace psxsplash {
namespace inputmap {

static constexpr int MAX_PLAYERS = 8;

/// Player 1 is port 1 (Pad1a) and player 2 port 2 (Pad2a), as before multitaps;
/// players 3-5 are port 1's tap slots B-D, players 6-8 port 2's. -1 when the
/// player is not 1..8.
constexpr int slotForPlayer(int player) {
    switch (player) {
        case 1: return 0;
        case 2: return 4;
        case 3: case 4: case 5: return player - 2;
        case 6: case 7: case 8: return player - 1;
        default: return -1;
    }
}

/// The inverse: the player (1..8) reading a slot (0..7), 0 for anything else.
constexpr int playerForSlot(int slot) {
    switch (slot) {
        case 0: return 1;
        case 4: return 2;
        case 1: case 2: case 3: return slot + 2;
        case 5: case 6: case 7: return slot + 1;
        default: return 0;
    }
}

/// 0 for port 1, 1 for port 2, -1 when the player is not 1..8.
constexpr int portForPlayer(int player) {
    int slot = slotForPlayer(player);
    return slot < 0 ? -1 : slot / 4;
}

enum class Device : uint8_t { None, Pad, Analog, Mouse, Other };

/// What a slot holds, from AdvancedPad's connected flag and pad type byte. The
/// type values are psyqo::AdvancedPad::PadType's (psx-spx's controller IDs).
constexpr Device classify(bool connected, uint8_t padType) {
    if (!connected) return Device::None;
    switch (padType) {
        case 0xff: return Device::None;    // PadType::None
        case 0x41: return Device::Pad;     // PadType::DigitalPad (or an analog pad in digital mode)
        case 0x73:                         // PadType::AnalogPad
        case 0x53: return Device::Analog;  // PadType::AnalogStick
        case 0x12: return Device::Mouse;   // PadType::Mouse
        default: return Device::Other;     // guns, NegCon, JogCon, config mode...
    }
}

/// Only an analog pad or stick has the four stick bytes; a mouse puts its motion
/// where the right stick would be, and guns their coordinates.
constexpr bool hasSticks(Device d) { return d == Device::Analog; }

/// Lua's Input.GetDevice names.
constexpr const char* deviceName(Device d) {
    switch (d) {
        case Device::Pad: return "pad";
        case Device::Analog: return "analog";
        case Device::Mouse: return "mouse";
        case Device::Other: return "other";
        default: return "none";
    }
}

/// The mouse's two buttons, as AdvancedPad::Button bit indices (they are where
/// L1 and R1 sit on a pad). psx-spx: the mouse's second button byte has bit 3 =
/// left, bit 2 = right, and bits 0-1 always 0 -- which read as L2 and R2 held.
static constexpr int MOUSE_LEFT_BIT = 11;
static constexpr int MOUSE_RIGHT_BIT = 10;
static constexpr uint16_t MOUSE_BUTTONS = (1u << MOUSE_LEFT_BIT) | (1u << MOUSE_RIGHT_BIT);

/// The buttons held, active-high, from the raw active-low button halfword. A
/// mouse keeps only its two buttons (its always-zero bits would otherwise hold
/// L2 and R2 down forever); nothing connected holds nothing.
constexpr uint16_t heldMask(Device d, uint16_t rawButtons) {
    if (d == Device::None) return 0;
    uint16_t held = static_cast<uint16_t>(~rawButtons);
    return d == Device::Mouse ? static_cast<uint16_t>(held & MOUSE_BUTTONS) : held;
}

/// Mouse motion summed over every poll since the last latch(): each poll adds
/// one report's signed 8-bit dx and dy (AdvancedPad's adc[0] and adc[1]).
struct MouseMotion {
    int32_t dx = 0;
    int32_t dy = 0;

    void addReport(uint8_t adcX, uint8_t adcY) {
        dx += static_cast<int8_t>(adcX);
        dy += static_cast<int8_t>(adcY);
    }
    /// Hands back the sum and starts the next one from zero.
    void latch(int32_t& outX, int32_t& outY) {
        outX = dx;
        outY = dy;
        dx = 0;
        dy = 0;
    }
};

/// The 0x42 read command that carries motor bytes to a port's slot A.
///
/// Without a tap it is the plain 01 42 00 small large. A multitap that has been
/// asked for its long read (psyqo's poll asks every time, with 01h as the third
/// byte) answers the next 01h access with all four slots, so the plain command
/// would land the motor bytes in slot A's id and TAP positions, and its 00h
/// would cancel the long read for psyqo's next poll, dropping slots B-D from
/// that frame. Through a tap the command is therefore a long read, asking for
/// the next one again, with slot A's 42h and TAP bytes before the motors.
/// Returns the length written to out (at most 7).
inline unsigned motorCommand(bool tap, uint8_t smallMotor, uint8_t largeMotor, uint8_t* out) {
    unsigned n = 0;
    out[n++] = 0x01;
    out[n++] = 0x42;
    if (tap) {
        out[n++] = 0x01;  // keep the long read requested
        out[n++] = 0x42;  // slot A's own read command
        out[n++] = 0x00;  // slot A's TAP byte
    } else {
        out[n++] = 0x00;
    }
    out[n++] = smallMotor;
    out[n++] = largeMotor;
    return n;
}

/// Whether a port's last poll came through a multitap, from AdvancedPad's
/// status byte (halfword 0, low byte) of the port's slots B-D: psyqo fills
/// them with FFh before every poll and only a multitap long read writes 00h
/// (connected) or 01h (empty) there. psyqo also writes 01h into all four slots
/// when the port does not answer at all, hence slot A must be connected too.
constexpr bool tapPresent(bool slotAConnected, uint8_t statusB, uint8_t statusC, uint8_t statusD) {
    return slotAConnected && (statusB != 0xff || statusC != 0xff || statusD != 0xff);
}

}  // namespace inputmap
}  // namespace psxsplash
