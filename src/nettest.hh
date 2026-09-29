#pragma once

#include <stdint.h>

namespace psxsplash {

struct NetTestResult {
    int passed;
    int failed;
};

// Runs the NetLink protocol against an in-RAM loopback (no hardware, no second
// console). Exercises framing, CRC validation, resync, the handshake packets,
// snapshots and the reliable event channel. Returns pass/fail counts.
NetTestResult runNetLinkSelfTest();

}  // namespace psxsplash
