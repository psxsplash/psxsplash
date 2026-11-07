// Host tests for player numbering, device classification, button masks, mouse
// motion and the motor command (src/inputmap.hh), compiled natively.
//
// The pad values are the ones psyqo's AdvancedPad reads from PCSX-Redux with a
// multitap and a mouse: a mouse's button halfword is FCFFh idle, F4FFh with the
// left button and F8FFh with the right; its motion halfword FB7Fh is dx 7Fh,
// dy FBh (-5).

#include "inputmap.hh"

#include "testing.hh"

using namespace psxsplash::inputmap;

TEST(players_1_and_2_are_the_ports) {
    CHECK_EQ(slotForPlayer(1), 0);  // Pad1a
    CHECK_EQ(slotForPlayer(2), 4);  // Pad2a
    CHECK_EQ(portForPlayer(1), 0);
    CHECK_EQ(portForPlayer(2), 1);
}

TEST(players_3_to_8_are_the_tap_slots) {
    CHECK_EQ(slotForPlayer(3), 1);  // Pad1b
    CHECK_EQ(slotForPlayer(4), 2);
    CHECK_EQ(slotForPlayer(5), 3);  // Pad1d
    CHECK_EQ(slotForPlayer(6), 5);  // Pad2b
    CHECK_EQ(slotForPlayer(7), 6);
    CHECK_EQ(slotForPlayer(8), 7);  // Pad2d
    for (int p = 3; p <= 5; p++) CHECK_EQ(portForPlayer(p), 0);
    for (int p = 6; p <= 8; p++) CHECK_EQ(portForPlayer(p), 1);
}

TEST(player_slot_mapping_is_a_bijection) {
    bool seen[8] = {};
    for (int p = 1; p <= MAX_PLAYERS; p++) {
        int s = slotForPlayer(p);
        CHECK(s >= 0 && s < 8);
        if (s < 0 || s >= 8) continue;
        CHECK(!seen[s]);
        seen[s] = true;
        CHECK_EQ(playerForSlot(s), p);
    }
}

TEST(players_out_of_range) {
    CHECK_EQ(slotForPlayer(0), -1);
    CHECK_EQ(slotForPlayer(9), -1);
    CHECK_EQ(slotForPlayer(-1), -1);
    CHECK_EQ(portForPlayer(0), -1);
    CHECK_EQ(playerForSlot(8), 0);
}

TEST(device_classification) {
    CHECK(classify(true, 0x41) == Device::Pad);
    CHECK(classify(true, 0x73) == Device::Analog);
    CHECK(classify(true, 0x53) == Device::Analog);
    CHECK(classify(true, 0x12) == Device::Mouse);
    CHECK(classify(true, 0x31) == Device::Other);  // Konami gun
    CHECK(classify(true, 0x63) == Device::Other);  // Namco gun
    CHECK(classify(true, 0x23) == Device::Other);  // NegCon
    CHECK(classify(true, 0xf3) == Device::Other);  // config mode
    CHECK(classify(true, 0xff) == Device::None);
    CHECK(classify(false, 0x41) == Device::None);  // a stale type on an empty slot
    CHECK(classify(false, 0x12) == Device::None);
}

TEST(only_analog_pads_have_sticks) {
    CHECK(hasSticks(classify(true, 0x73)));
    CHECK(hasSticks(classify(true, 0x53)));
    const uint8_t noSticks[] = {0x12, 0x23, 0x31, 0x41, 0x63, 0xe3, 0xe5, 0xf3, 0xff};
    for (uint8_t t : noSticks) CHECK(!hasSticks(classify(true, t)));
    CHECK(!hasSticks(Device::None));
}

TEST(device_names) {
    CHECK(__builtin_strcmp(deviceName(Device::None), "none") == 0);
    CHECK(__builtin_strcmp(deviceName(Device::Pad), "pad") == 0);
    CHECK(__builtin_strcmp(deviceName(Device::Analog), "analog") == 0);
    CHECK(__builtin_strcmp(deviceName(Device::Mouse), "mouse") == 0);
    CHECK(__builtin_strcmp(deviceName(Device::Other), "other") == 0);
}

TEST(pad_buttons_are_inverted) {
    CHECK_EQ(heldMask(Device::Pad, 0xffff), 0);
    CHECK_EQ(heldMask(Device::Pad, 0xbfff), 1 << 14);  // Cross
    CHECK_EQ(heldMask(Device::Analog, 0x7fff), 1 << 15);  // Square
    CHECK_EQ(heldMask(Device::None, 0x0000), 0);
}

TEST(mouse_buttons) {
    CHECK_EQ(MOUSE_LEFT_BIT, 11);
    CHECK_EQ(MOUSE_RIGHT_BIT, 10);
    CHECK_EQ(heldMask(Device::Mouse, 0xfcff), 0);  // idle: no L2/R2 from the zero bits
    CHECK_EQ(heldMask(Device::Mouse, 0xf4ff), 1 << MOUSE_LEFT_BIT);
    CHECK_EQ(heldMask(Device::Mouse, 0xf8ff), 1 << MOUSE_RIGHT_BIT);
    CHECK_EQ(heldMask(Device::Mouse, 0xf0ff), (1 << MOUSE_LEFT_BIT) | (1 << MOUSE_RIGHT_BIT));
}

TEST(mouse_motion_is_signed_and_summed) {
    MouseMotion m;
    m.addReport(0x7f, 0xfb);  // +127, -5
    m.addReport(0x7f, 0x00);
    m.addReport(0x2e, 0x00);  // 300 = 127 + 127 + 46
    int32_t dx = -1, dy = -1;
    m.latch(dx, dy);
    CHECK_EQ(dx, 300);
    CHECK_EQ(dy, -5);
    m.addReport(0xf9, 0x80);  // -7, -128
    m.latch(dx, dy);
    CHECK_EQ(dx, -7);
    CHECK_EQ(dy, -128);
}

TEST(mouse_motion_resets_at_latch) {
    MouseMotion m;
    m.addReport(0x10, 0x20);
    int32_t dx, dy;
    m.latch(dx, dy);
    m.latch(dx, dy);
    CHECK_EQ(dx, 0);
    CHECK_EQ(dy, 0);
}

TEST(motor_command_without_tap) {
    uint8_t cmd[8];
    unsigned n = motorCommand(false, 1, 0xc0, cmd);
    const uint8_t want[] = {0x01, 0x42, 0x00, 0x01, 0xc0};
    CHECK_EQ(n, sizeof(want));
    CHECK_MEM(cmd, want, sizeof(want));
}

TEST(motor_command_through_tap) {
    uint8_t cmd[8];
    unsigned n = motorCommand(true, 1, 0xc0, cmd);
    // A long read that asks for the next one, slot A's 42h and TAP bytes, motors.
    const uint8_t want[] = {0x01, 0x42, 0x01, 0x42, 0x00, 0x01, 0xc0};
    CHECK_EQ(n, sizeof(want));
    CHECK_MEM(cmd, want, sizeof(want));
}

TEST(tap_detection) {
    // No tap: psyqo leaves slots B-D at FFh.
    CHECK(!tapPresent(true, 0xff, 0xff, 0xff));
    // A tap: slots B-D are 00h (connected) or 01h (empty).
    CHECK(tapPresent(true, 0x00, 0x01, 0x00));
    CHECK(tapPresent(true, 0x01, 0x01, 0x01));
    // Nothing answering on the port: psyqo writes 01h in all four slots.
    CHECK(!tapPresent(false, 0x01, 0x01, 0x01));
}

int main() { return psxsplash::test::runAll(); }
