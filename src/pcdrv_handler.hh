#pragma once

#include <stdint.h>
#include <psyqo/kernel.hh>
#include "common/hardware/pcsxhw.h"
#include "common/kernel/pcdrv.h"

namespace psxsplash {

// =========================================================================
// SIO1 hardware registers (UART serial port at 0x1F801050)
// =========================================================================

#define SIO1_DATA  (*(volatile uint8_t*)0x1F801050)
#define SIO1_STAT  (*(volatile uint32_t*)0x1F801054)
#define SIO1_MODE  (*(volatile uint16_t*)0x1F801058)
#define SIO1_CTRL  (*(volatile uint16_t*)0x1F80105A)
#define SIO1_BAUD  (*(volatile uint16_t*)0x1F80105E)

#define SIO1_TX_RDY (1 << 0)
#define SIO1_RX_RDY (1 << 1)

// =========================================================================
// Low-level SIO1 I/O - blocking, tight polling loops
// =========================================================================

static inline void sio_putc(uint8_t byte) {
    while (!(SIO1_STAT & SIO1_TX_RDY)) {}
    SIO1_DATA = byte;
}

static inline uint8_t sio_getc() {
    while (!(SIO1_STAT & SIO1_RX_RDY)) {}
    return SIO1_DATA;
}

static inline void sio_write32(uint32_t val) {
    sio_putc((uint8_t)(val));
    sio_putc((uint8_t)(val >> 8));
    sio_putc((uint8_t)(val >> 16));
    sio_putc((uint8_t)(val >> 24));
}

static inline uint32_t sio_read32() {
    uint32_t v  = (uint32_t)sio_getc();
    v |= (uint32_t)sio_getc() << 8;
    v |= (uint32_t)sio_getc() << 16;
    v |= (uint32_t)sio_getc() << 24;
    return v;
}

static inline bool sio_check_okay() {
    return sio_getc() == 'O' && sio_getc() == 'K'
        && sio_getc() == 'A' && sio_getc() == 'Y';
}

static inline void sio_pcdrv_escape(uint32_t funcCode) {
    sio_putc(0x00);
    sio_putc('p');
    sio_write32(funcCode);
}


static int sio_pcdrv_init() {
    sio_pcdrv_escape(0x101);
    if (sio_check_okay()) {
        sio_getc();  // trailing 0x00
        return 0;
    }
    return -1;
}

static int sio_pcdrv_open(const char* name, int flags) {
    sio_pcdrv_escape(0x103);
    if (!sio_check_okay()) return -1;
    const char* p = name;
    while (*p) sio_putc((uint8_t)*p++);
    sio_putc(0x00);
    sio_write32((uint32_t)flags);
    if (sio_check_okay()) {
        return (int)sio_read32();  // handle
    }
    return -1;
}

static int sio_pcdrv_close(int fd) {
    sio_pcdrv_escape(0x104);
    if (!sio_check_okay()) return -1;
    sio_write32((uint32_t)fd);
    sio_write32(0);  // unused
    sio_write32(0);  // unused
    if (sio_check_okay()) {
        sio_read32();  // handle echo
        return 0;
    }
    return -1;
}

static int sio_pcdrv_read(int fd, void* buf, int len) {
    sio_pcdrv_escape(0x105);
    if (!sio_check_okay()) return -1;
    sio_write32((uint32_t)fd);
    sio_write32((uint32_t)len);
    sio_write32((uint32_t)(uintptr_t)buf);  // memaddr for host debug
    if (sio_check_okay()) {
        uint32_t dataLen = sio_read32();
        sio_read32();  // checksum (not verified)
        uint8_t* dst = (uint8_t*)buf;
        for (uint32_t i = 0; i < dataLen; i++) {
            dst[i] = sio_getc();
        }
        return (int)dataLen;
    }
    return -1;
}

static int sio_pcdrv_seek(int fd, int offset, int whence) {
    sio_pcdrv_escape(0x107);
    if (!sio_check_okay()) return -1;
    sio_write32((uint32_t)fd);
    sio_write32((uint32_t)offset);
    sio_write32((uint32_t)whence);
    if (sio_check_okay()) {
        return (int)sio_read32();  // new position
    }
    return -1;
}

// =========================================================================
// Public PCDRV API - runtime dispatch between emulator and real hardware
// Use these instead of pcdrv.h functions (PCopen, PCread, etc.)
// =========================================================================

static int pcdrv_init() {
    if (pcsx_present()) return PCinit();
    return sio_pcdrv_init();
}

static int pcdrv_open(const char* name, int flags, int perms) {
    if (pcsx_present()) return PCopen(name, flags, perms);
    return sio_pcdrv_open(name, flags);
}

static int pcdrv_close(int fd) {
    if (pcsx_present()) return PCclose(fd);
    return sio_pcdrv_close(fd);
}

static int pcdrv_read(int fd, void* buf, int len) {
    if (pcsx_present()) return PCread(fd, buf, len);
    return sio_pcdrv_read(fd, buf, len);
}

static int pcdrv_seek(int fd, int offset, int whence) {
    if (pcsx_present()) return PClseek(fd, offset, whence);
    return sio_pcdrv_seek(fd, offset, whence);
}

// =========================================================================
// SIO1 initialization - 115200 baud, 8N1
// =========================================================================

static void sio1Init() {
    SIO1_CTRL = 0;                                       // reset
    SIO1_MODE = 0x004e;                                  // MUL16, 8 data, no parity, 1 stop
    SIO1_BAUD = (uint16_t)(2073600 / 115200);            // = 18
    SIO1_CTRL = 0x0025;                                  // TX enable, RX enable, RTS assert
    for (int i = 0; i < 100; i++) { __asm__ volatile("" ::: "memory"); }  // settle delay
}


static void pcdrv_sio1_init() {
    if (pcsx_present()) return;  // emulator handles PCDRV natively

    sio1Init();

    // TODO: printf redirect (redirectPrintfToSIO1) disabled for now.
    // Printf redirect patches A0 handler machine code at 0xa8/0xb4
    // and may cause instability - needs further testing.
}

}  // namespace psxsplash
