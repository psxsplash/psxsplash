#pragma once

#include <stdint.h>
#include <psyqo/fixed-point.hh>

namespace psxsplash {

/**
 * Interactable component - enables player interaction with objects.
 *
 * When the player is within interaction radius and presses the interact button,
 * the onInteract Lua event fires on the associated GameObject.
 */
struct Interactable {
    // Interaction radius squared (fixed-point 12-bit, pre-squared for fast distance checks)
    psyqo::FixedPoint<12> radiusSquared;

    // Button index that triggers interaction (0-15, matches psyqo::AdvancedPad::Button)
    uint8_t interactButton;

    // Configuration flags
    uint8_t flags;  // bit 0: isRepeatable, bit 1: showPrompt, bit 2: requireLineOfSight, bit 3: disabled

    // Cooldown between interactions (in frames)
    uint16_t cooldownFrames;

    // Runtime state
    uint16_t currentCooldown;  // Frames remaining until can interact again
    uint16_t gameObjectIndex;  // Index of associated GameObject

    // Prompt canvas name (null-terminated, max 15 chars + null)
    char promptCanvasName[16];

    // Flag accessors
    bool isRepeatable() const { return flags & 0x01; }
    bool showPrompt() const { return flags & 0x02; }
    bool requireLineOfSight() const { return flags & 0x04; }
    bool isDisabled() const { return flags & 0x08; }
    void setDisabled(bool disabled) {
        if (disabled) flags |= 0x08;
        else flags &= ~0x08;
    }

    // Check if ready to interact
    bool canInteract() const {
        // Non-repeatable interactions: once cooldown was set, it stays permanently
        if (!isRepeatable() && cooldownFrames > 0 && currentCooldown == 0) {
            // Check if we already triggered once (cooldownFrames acts as sentinel)
            // We use a special value to mark "already used"
        }
        return currentCooldown == 0;
    }

    // Called when interaction happens
    void triggerCooldown() {
        if (isRepeatable()) {
            currentCooldown = cooldownFrames;
        } else {
            // Non-repeatable: set to max to permanently disable
            currentCooldown = 0xFFFF;
        }
    }

    // Called each frame to decrement cooldown
    void update() {
        if (currentCooldown > 0 && currentCooldown != 0xFFFF) currentCooldown--;
    }
};
static_assert(sizeof(Interactable) == 28, "Interactable must be 28 bytes");

}  // namespace psxsplash
