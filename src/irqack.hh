#pragma once

#include <stdint.h>

#include <psyqo/hardware/cpu.hh>
#include <psyqo/hardware/hwregs.hh>

namespace psxsplash {

/**
 * Acknowledge ONE interrupt in I_STAT without touching any other.
 *
 * THIS IS THE ONLY SAFE WAY TO ACKNOWLEDGE AN INTERRUPT ON THIS MACHINE, and
 * getting it wrong does not produce a serial bug or a CD bug - it produces
 * whichever one happens to lose the race, which is why it took so long to see.
 *
 * I_STAT is acknowledge-by-writing-ZERO: a zero written to a bit acknowledges it,
 * a one leaves it pending. psyqo's `IReg.clear(irq)` is `*this &= ~irq`, and
 * `operator&=` (hwregs.hh) is a READ-MODIFY-WRITE:
 *
 *     tmp = I_STAT      // any bit not yet pending reads as 0
 *        <-- another device asserts: hardware sets its bit
 *     tmp &= ~irq
 *     I_STAT = tmp      // that bit is written back as 0 -> ACKNOWLEDGED AND LOST
 *
 * So every caller of `IReg.clear()` can destroy any other device's pending
 * interrupt. It is not one driver's bug, it is a mutual one: this tree had four
 * such call sites - the SIO1 handler, the SIO1 re-arm, the CD-ROM drain and the
 * controller ack - each able to silently swallow the others' interrupts.
 *
 * The consequences are asymmetric and both bad:
 *   - Lose the SIO1 bit and the receive interrupt is dead for the session, because
 *     SIO_STAT.9 is sticky while I_STAT.8 is edge-triggered, so no further edge is
 *     ever generated (psx-spx, interrupts.md). That is what Sio1::rearmRx() exists
 *     to undo.
 *   - Lose the CD-ROM bit and a psyqo CDRomDevice operation never completes: it is
 *     an asynchronous state machine driven by IRQ2, so the callback simply never
 *     arrives and whatever was waiting on it waits forever. A silent freeze with
 *     the last frame still on screen, correlated with drive activity and nothing
 *     else - which is exactly how it presented.
 *
 * WRITING NEEDS NO READ. `I_STAT = ~bit` writes a zero to the one bit being
 * acknowledged and a one to every other, which is precisely "acknowledge this,
 * leave everything else pending". With no read there is no window, so this is
 * immune by construction rather than by timing - and it is cheaper than the
 * critical section that was the first fix for it, not merely safer.
 *
 * Safe from an interrupt handler and from the main thread alike: it is a single
 * store with no state and no re-entrancy of its own.
 */
inline void ackIrq(psyqo::Hardware::CPU::IRQ irq) {
    // Same register psyqo's IReg addresses (0x1F801070), same width, so this is a
    // drop-in for IReg.clear() rather than a second view of the hardware.
    psyqo::Hardware::Register<0x0070> istat;
    istat = ~static_cast<uint32_t>(irq);
}

}  // namespace psxsplash
