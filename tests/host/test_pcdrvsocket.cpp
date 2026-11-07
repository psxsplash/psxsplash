// Host tests for PcdrvSocket (src/pcdrvsocket.cpp), compiled natively.
//
// The PCDRV host is faked here with the contract psxmon and PCSX-Redux share:
// "probe:net" opens only with networking on, "tcp://host:port" opens a stream,
// PCread never blocks (0 = nothing yet, < 0 = peer gone), PCwrite writes all.

#include <string.h>

#include "pcdrvsocket.hh"
#include "mocktransport.hh"
#include "netlink.hh"
#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::net;
using namespace psxsplash::test;

namespace {

// Two stream fds whose ends are crossed, so two sockets form one connection.
constexpr int c_fdA = 3, c_fdB = 4, c_fdProbe = 7;

struct FakeHost {
    bool netEnabled = true;
    bool connectOk = true;
    int opens = 0, probeCloses = 0, closes = 0;
    char lastName[96] = {};
    int nextStream = c_fdA;
    // in[i] holds the bytes waiting to be read on stream fd c_fdA + i.
    RingBuffer<8192> in[2];
    int readLimit = 1 << 30;  // most bytes one PCread returns
    bool readFails = false;
    int writeShort = -1;  // >= 0: the next PCwrite writes only this many
    uint16_t clock = 0;   // scanlines, moved by the calls and by the tests' frames
    uint16_t callCost = 0;
    int reads = 0, writes = 0;
    uint32_t busy = 0;
};
FakeHost g;

void pay() { g.clock += g.callCost; g.busy += g.callCost; }

int fakeOpen(const char* name) {
    g.opens++;
    strncpy(g.lastName, name, sizeof(g.lastName) - 1);
    if (strcmp(name, "probe:net") == 0) return g.netEnabled ? c_fdProbe : -1;
    if (!g.netEnabled || strncmp(name, "tcp://", 6) != 0 || !g.connectOk) return -1;
    return g.nextStream++;
}
int fakeClose(int fd) {
    if (fd == c_fdProbe) g.probeCloses++;
    else g.closes++;
    return 0;
}
int fakeRead(int fd, void* buf, int len) {
    g.reads++;
    pay();
    if (g.readFails) return -1;
    auto& ring = g.in[fd - c_fdA];
    int n = 0;
    uint8_t* dst = static_cast<uint8_t*>(buf);
    while (n < len && n < g.readLimit && ring.pop(dst[n])) n++;
    return n;
}
int fakeWrite(int fd, const void* buf, int len) {
    g.writes++;
    pay();
    if (g.writeShort >= 0) {
        len = g.writeShort;
        g.writeShort = -1;
    }
    auto& peer = g.in[(fd == c_fdA ? c_fdB : c_fdA) - c_fdA];
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    for (int i = 0; i < len; i++) peer.push(src[i]);
    return len;
}
uint16_t fakeNow() { return g.clock; }
const PcdrvNetOps c_ops = {fakeOpen, fakeClose, fakeRead, fakeWrite, nullptr};
const PcdrvNetOps c_timedOps = {fakeOpen, fakeClose, fakeRead, fakeWrite, fakeNow};

void resetHost() { g = FakeHost{}; }

// A frame's worth of a game: one poll, one byte queued, 16.7 ms in scanlines.
constexpr uint16_t c_frameLines = 262;
void playFrames(PcdrvSocket& s, int frames) {
    for (int i = 0; i < frames; i++) {
        const uint8_t b = 0x55;
        s.writeAll(&b, 1);
        s.poll();
        g.clock += c_frameLines;
    }
}

void feed(int fd, const uint8_t* p, int n) {
    for (int i = 0; i < n; i++) g.in[fd - c_fdA].push(p[i]);
}

}  // namespace

TEST(no_host_networking_means_no_socket) {
    resetHost();
    g.netEnabled = false;
    PcdrvSocket s(c_ops);
    CHECK(!s.open("127.0.0.1:6699"));
    CHECK(!s.isOpen());
    CHECK_EQ(g.opens, 1);  // the probe only; tcp:// is never tried
}

TEST(probe_then_connect_with_the_tcp_name) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("10.0.0.2:7000"));
    CHECK(s.isOpen());
    CHECK_EQ(g.probeCloses, 1);
    CHECK(strcmp(g.lastName, "tcp://10.0.0.2:7000") == 0);
}

TEST(refused_connect_leaves_the_socket_closed) {
    resetHost();
    g.connectOk = false;
    PcdrvSocket s(c_ops);
    CHECK(!s.open(PcdrvSocket::c_defaultServer));
    CHECK(!s.isOpen());
    CHECK_EQ(g.probeCloses, 1);
    CHECK(strcmp(g.lastName, "tcp://127.0.0.1:6699") == 0);
}

TEST(overlong_or_empty_server_is_refused_before_any_call) {
    resetHost();
    PcdrvSocket s(c_ops);
    char longName[PcdrvSocket::c_maxServerLen + 2];
    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = 0;
    CHECK(!s.open(longName));
    CHECK(!s.open(""));
    CHECK(!s.open(nullptr));
    CHECK_EQ(g.opens, 0);
}

TEST(open_again_keeps_the_connection) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("127.0.0.1:6699"));
    CHECK(s.open("127.0.0.1:6699"));
    CHECK_EQ(g.opens, 2);  // one probe + one connect, not four
}

TEST(nothing_moves_until_poll_and_poll_never_waits) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    const uint8_t msg[3] = {1, 2, 3};
    CHECK(s.writeAll(msg, 3));
    CHECK_EQ(g.in[1].size(), 0);  // queued, not yet written
    s.poll();                     // nothing has arrived: returns at once
    CHECK_EQ(g.in[1].size(), 3);
    CHECK_EQ(s.available(), 0);
    CHECK_EQ(s.bytesSent(), 3);
}

TEST(poll_takes_everything_that_has_arrived_across_short_reads) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    uint8_t data[700];
    for (int i = 0; i < 700; i++) data[i] = static_cast<uint8_t>(i * 7);
    feed(c_fdA, data, 700);
    s.poll();  // 256 + 256 + 188: the short read ends the loop
    CHECK_EQ(s.available(), 700);
    uint8_t out[700];
    CHECK_EQ(s.read(out, sizeof(out)), 700);
    CHECK_MEM(out, data, 700);
    CHECK_EQ(s.bytesReceived(), 700);
}

TEST(write_all_is_all_or_nothing) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    uint8_t big[1024] = {};
    CHECK(s.writeAll(big, 1000));
    CHECK(!s.writeAll(big, 100));  // 24 left
    CHECK_EQ(s.txSpace(), 24);
    CHECK(s.txCongestion() > 240);
    s.poll();
    CHECK_EQ(s.txSpace(), s.txCapacity());
    CHECK_EQ(g.in[1].size(), 1000);
}

TEST(closed_socket_refuses_writes) {
    resetHost();
    PcdrvSocket s(c_ops);
    const uint8_t b = 1;
    CHECK(!s.writeAll(&b, 1));
    CHECK_EQ(s.write(&b, 1), 0);
}

TEST(peer_gone_on_read_closes_and_drops_queues) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    const uint8_t msg[4] = {9, 9, 9, 9};
    CHECK(s.writeAll(msg, 4));
    g.readFails = true;
    s.poll();
    CHECK(!s.isOpen());
    CHECK_EQ(g.closes, 1);
    CHECK_EQ(s.txSpace(), s.txCapacity());
    CHECK_EQ(g.in[1].size(), 0);  // the queued bytes never went out
}

TEST(short_write_closes_rather_than_corrupting_the_stream) {
    resetHost();
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    const uint8_t msg[10] = {};
    CHECK(s.writeAll(msg, 10));
    g.writeShort = 4;
    s.poll();
    CHECK(!s.isOpen());
    // And it can be reopened afterwards.
    CHECK(s.open("h:1"));
    CHECK(s.isOpen());
}

TEST(netlink_runs_unchanged_over_two_sockets) {
    resetHost();
    g.readLimit = 5;  // bytes dribble in, as they would over a real socket
    PcdrvSocket sa(c_ops), sb(c_ops);
    CHECK(sa.open("h:1"));
    CHECK(sb.open("h:1"));
    RecordingHandler ha, hb;
    NetLink la(sa, ha), lb(sb, hb);
    la.setLocalSlot(1);
    lb.setLocalSlot(0);
    const uint8_t ev[6] = {'P', 'C', 'D', 'R', 'V', '!'};
    CHECK(la.sendReliable(PacketType::Event, ev, 6));
    const uint8_t app[3] = {7, 8, 9};
    CHECK(lb.sendReliable(PacketType::AppData, app, 3));
    for (int i = 0; i < 40; i++) {
        la.poll(4096);
        lb.poll(4096);
    }
    CHECK_EQ(hb.eventCount, 1);
    CHECK_MEM(hb.lastEvent, ev, 6);
    CHECK_EQ(hb.lastEventSlot, 1);
    CHECK_EQ(ha.appDataCount, 1);
    CHECK_MEM(ha.lastAppData, app, 3);
    CHECK(la.reliableIdle());
    CHECK(lb.reliableIdle());
}

TEST(netlink_switches_transport) {
    resetHost();
    MockTransport::Ring r1, r2;
    MockTransport mock(r1, r2);
    PcdrvSocket s(c_ops);
    CHECK(s.open("h:1"));
    RecordingHandler h;
    NetLink l(mock, h);
    CHECK(&l.transport() == &mock);
    l.setTransport(s);
    l.reset();
    CHECK(&l.transport() == &s);
    CHECK(l.send(PacketType::Ping, nullptr, 0));
    CHECK_EQ(r1.size(), 0);   // nothing went to the old transport
    CHECK(s.txSpace() < s.txCapacity());
}

// A game held to the video: its work and any calls, then a wait for the next frame.
uint32_t playLockedFrames(PcdrvSocket& s, int frames, uint16_t work, bool send = true) {
    uint32_t total = 0;
    for (int i = 0; i < frames; i++) {
        const uint16_t start = g.clock;
        const uint8_t b = 0x55;
        if (send) s.writeAll(&b, 1);
        s.poll();
        const uint32_t used = static_cast<uint16_t>(g.clock - start) + work;
        const uint32_t frame = (used + c_frameLines - 1) / c_frameLines * c_frameLines;
        g.clock = static_cast<uint16_t>(start + frame);
        total += frame;
    }
    return total;
}

constexpr uint32_t c_linesPerSecond = 15734;

// A scene that takes 72 ms of a frame's work, as the courtyard on an SCPH-5501: 12.5 fps
// alone, 7.1 with a read and a write every frame at 115200, 11.0 paced, and about two reads
// and two writes a second. The floors here are what keep it from being paced into silence.
TEST(courtyard_at_115200_is_paced_and_keeps_talking) {
    resetHost();
    g.callCost = 450;  // 28.6 ms, between the console's PCread and PCwrite
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playLockedFrames(s, 20, 1130);
    const int reads = g.reads, writes = g.writes;
    const int frames = 1000;
    const uint32_t lines = playLockedFrames(s, frames, 1130);
    const uint32_t tenthsFps = frames * 10ull * c_linesPerSecond / lines;
    const uint32_t r = (g.reads - reads) * 10ull * c_linesPerSecond / lines;
    const uint32_t w = (g.writes - writes) * 10ull * c_linesPerSecond / lines;
    CHECK(tenthsFps >= 95);  // over the 7.1 of a call each way every frame, and near the 12 alone
    CHECK(r >= 10);          // a second of the session never passes without a read
    CHECK(w >= 10);          // or a write
}

TEST(courtyard_at_529200_keeps_talking) {
    resetHost();
    g.callCost = 120;  // 7.6 ms
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playLockedFrames(s, 20, 1240);
    const int reads = g.reads, writes = g.writes;
    const int frames = 1000;
    const uint32_t lines = playLockedFrames(s, frames, 1240);
    CHECK((g.reads - reads) * 1ull * c_linesPerSecond / lines >= 2);
    CHECK((g.writes - writes) * 1ull * c_linesPerSecond / lines >= 2);
}

// Nothing to send: every turn is a read, so a quiet scene still hears the server.
TEST(a_scene_with_nothing_to_send_still_reads) {
    resetHost();
    g.callCost = 450;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playLockedFrames(s, 20, 1130, false);
    const int reads = g.reads;
    const int frames = 1000;
    const uint32_t lines = playLockedFrames(s, frames, 1130, false);
    CHECK_EQ(g.writes, 0);
    CHECK((g.reads - reads) * 1ull * c_linesPerSecond / lines >= 2);
}

// What the server sends has to be taken as it comes, and the game's frames go on, whatever comes.
uint32_t playServedFrames(PcdrvSocket& s, int frames, uint16_t work, int perFrame) {
    uint32_t total = 0;
    uint8_t in[512];
    for (int i = 0; i < frames; i++) {
        for (int k = 0; k < perFrame; k++) g.in[0].push(0x5A);
        total += playLockedFrames(s, 1, work);   // sending too: a session's reads and writes take turns
        while (s.read(in, sizeof(in)) > 0) {}
    }
    return total;
}

// psnl_server's 30 snapshots a second to one console, 1,068 B/s, in the courtyard at 115200. At 256
// bytes a read the SCPH-5501 took 614 B/s of that and was 50 KB behind after 112 s: every read full.
TEST(a_server_that_sends_more_than_a_turn_takes_is_kept_up_with) {
    resetHost();
    g.callCost = 760;  // 48 ms, the console's PCread of 256 bytes at 115200
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playServedFrames(s, 20, 1130, 89);
    playServedFrames(s, 2000, 1130, 89);
    CHECK(g.in[0].size() < 1024);
}

// More than the link can carry: the socket falls behind, and the game keeps its half.
TEST(a_server_that_floods_the_link_leaves_the_game_half_its_time) {
    resetHost();
    g.callCost = 760;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    const int frames = 1000;
    const uint32_t lines = playServedFrames(s, frames, 1130, 400);
    const uint32_t alone = frames * 1310u;   // 1,130 of work, five video frames
    CHECK(lines <= alone * 2 + alone / 4);
}

// One long frame is the scene's and not the socket's: the turn it happened to carry is charged
// for what a turn can cost, so the calls are back within ten frames and not half a second on.
TEST(a_long_frame_is_not_charged_to_the_socket) {
    resetHost();
    g.callCost = 450;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playLockedFrames(s, 40, 100);
    while (true) {                       // to the frame after a turn
        const int calls = g.reads + g.writes;
        playLockedFrames(s, 1, 100);
        if (g.reads + g.writes != calls) break;
    }
    int calls = g.reads + g.writes;
    while (g.reads + g.writes == calls) playLockedFrames(s, 1, 12000);
    calls = g.reads + g.writes;
    int frames = 0;
    while (g.reads + g.writes == calls && frames < 100) { playLockedFrames(s, 1, 100); frames++; }
    CHECK(frames <= 10);
}

// A scene that does that all the time: every eighth frame takes 0.76 s. poll() comes once a
// frame, so a turn can't come more often than the frames let it. What must hold is that the
// session is never left silent.
TEST(a_scene_that_stalls_cannot_starve_the_socket) {
    resetHost();
    g.callCost = 450;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playLockedFrames(s, 20, 100);
    const int calls = g.reads + g.writes;
    uint32_t lines = 0, gap = 0, worst = 0;
    for (int i = 0; i < 1600; i++) {
        const int before = g.reads + g.writes;
        const uint32_t frame = playLockedFrames(s, 1, i % 8 == 7 ? 12000 : 100);
        lines += frame;
        gap = g.reads + g.writes == before ? gap + frame : 0;
        if (gap > worst) worst = gap;
    }
    CHECK((g.reads + g.writes - calls) * 1ull * c_linesPerSecond / lines >= 1);
    CHECK(worst <= 2 * c_linesPerSecond);
    CHECK(g.reads > 0 && g.writes > 0);
}

// 6.9 ms: the same PCread at 529200, in a game with most of its frame to spare.
TEST(calls_that_fit_in_the_frame_are_not_rationed) {
    resetHost();
    g.callCost = 110;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playLockedFrames(s, 20, 50);  // the first turn teaches it what a quiet frame is
    const int before = g.reads + g.writes;
    playLockedFrames(s, 500, 50);
    CHECK(g.reads + g.writes - before >= 495);
}

TEST(calls_that_miss_the_frame_are_rationed) {
    resetHost();
    g.callCost = 110;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playLockedFrames(s, 20, 200);  // 200 + 110 is over the 262 of a frame
    const int before = g.reads + g.writes;
    playLockedFrames(s, 500, 200);
    CHECK(g.reads + g.writes - before <= 180);
    CHECK(g.reads + g.writes - before >= 60);
}

TEST(fast_host_is_served_both_ways_every_poll) {
    resetHost();
    g.callCost = 1;  // 64 us
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playFrames(s, 20);
    const int reads = g.reads, writes = g.writes;
    playFrames(s, 500);
    CHECK_EQ(g.reads - reads, 500);
    CHECK_EQ(g.writes - writes, 500);
}

// 25.7 ms: an empty PCread through nugget's monitor at 115200 on an SCPH-5501.
TEST(slow_host_never_costs_more_than_a_quarter) {
    resetHost();
    g.callCost = 404;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    const uint16_t start = g.clock;
    uint32_t total = 0;
    for (int i = 0; i < 2000; i++) {
        const uint16_t before = g.clock;
        playFrames(s, 1);
        total += static_cast<uint16_t>(g.clock - before);
    }
    (void)start;
    CHECK(g.busy * 100 <= total * 26);
}

TEST(slow_host_gets_one_call_at_a_time_and_both_ways) {
    resetHost();
    g.callCost = 404;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playFrames(s, 2000);
    CHECK(g.reads > 50);
    CHECK(g.writes > 50);
    const int gap = g.reads - g.writes;
    CHECK(gap >= -2 && gap <= 2);
}

TEST(a_pause_longer_than_the_clock_does_not_stop_the_socket) {
    resetHost();
    g.callCost = 404;
    PcdrvSocket s(c_timedOps);
    CHECK(s.open("127.0.0.1:6699"));
    playFrames(s, 100);
    const int reads = g.reads, writes = g.writes;
    g.clock += 40000;  // a scene load
    g.clock += 40000;
    playFrames(s, 100);
    CHECK(g.reads + g.writes > reads + writes);
}

int main() { return psxsplash::test::runAll(); }
