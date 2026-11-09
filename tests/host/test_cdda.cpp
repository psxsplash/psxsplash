// Host tests for the CD-DA play / loop / cancel bookkeeping (src/cddastate.hh),
// compiled natively: what MusicManager sends the drive, and when.
//
// psyqo's play callback fires twice with true: once when the track starts, and
// once when it is over, for a track that ran out and for one paused or stopped
// alike. The fake drive below does the same.

#include "cddastate.hh"

#include "testing.hh"

using psxsplash::CDDAState;
using Action = CDDAState::Action;
using Phase = CDDAState::Phase;

namespace {

// psyqo's CDRomDevice, as far as CD-DA goes: busy from a play until its second
// callback, which also ends a pause or stop.
struct FakeDrive {
    CDDAState& s;
    bool busy = false;
    int plays = 0;
    int lastTrack = -1;
    int pauses = 0;
    int stops = 0;
    int silences = 0;

    bool idle() const { return !busy; }
    void startPlay() {
        busy = true;
        plays++;
        lastTrack = s.track();
    }
    void started() { s.callback(true); }   // the first callback
    void over() {                           // the second: end, pause or stop done
        busy = false;
        s.callback(true);
    }
    void tick() {
        switch (s.tick(idle())) {
            case Action::Play: startPlay(); break;
            case Action::Pause: pauses++; break;
            case Action::Stop: stops++; break;
            case Action::Silence: silences++; break;
            case Action::None: break;
        }
    }
    void play(int track, bool loop) {
        if (s.play(track, loop, idle())) startPlay();
    }
};

}  // namespace

TEST(plain_play_runs_once) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, false);
    CHECK_EQ(d.plays, 1);
    d.started();
    CHECK(s.playing());
    d.over();  // ran to its end
    CHECK(!s.playing());
    d.tick();
    d.tick();
    CHECK_EQ(d.plays, 1);
    CHECK_EQ(d.silences, 1);  // the drive goes to sleep once, as before
    CHECK_EQ(s.track(), -1);
}

TEST(looping_track_comes_round_again) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    for (int round = 1; round <= 3; round++) {
        CHECK_EQ(d.plays, round);
        CHECK_EQ(d.lastTrack, 2);
        d.started();
        d.tick();  // playing: nothing to do
        CHECK_EQ(d.plays, round);
        d.over();  // the end of the track
        d.tick();  // next frame: the same track again
    }
    CHECK_EQ(d.plays, 4);
    CHECK_EQ(d.silences, 0);
    CHECK(s.looping());
}

TEST(stop_ends_the_loop) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    d.started();
    CHECK(s.stop());  // send STOP
    d.over();         // the stop's completion is the second callback
    d.tick();
    d.tick();
    CHECK_EQ(d.plays, 1);
    CHECK_EQ(d.silences, 1);
    CHECK(!s.looping());
}

TEST(pause_ends_the_loop_and_resume_plays_once) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    d.started();
    CHECK(s.pause());
    d.over();
    d.tick();
    CHECK_EQ(d.plays, 1);
    CHECK_EQ(d.silences, 0);  // a pause keeps the drive for a resume
    CHECK_EQ(s.track(), 2);
    CHECK(s.resume(d.idle()));
    d.busy = true;
    d.started();
    CHECK(s.playing());
    d.over();  // the resumed track ran out
    d.tick();
    CHECK_EQ(d.plays, 1);
    CHECK_EQ(d.silences, 1);
}

TEST(stop_while_starting_stops_once_it_plays) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    CHECK(!s.stop());  // psyqo refuses a STOP before the track plays
    d.tick();
    CHECK_EQ(d.stops, 0);
    d.started();
    d.tick();
    CHECK_EQ(d.stops, 1);
    d.over();
    d.tick();
    CHECK_EQ(d.plays, 1);
    CHECK_EQ(d.silences, 1);
}

TEST(new_play_replaces_the_loop) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    d.started();
    d.play(3, false);  // the drive is busy: queued
    CHECK_EQ(d.plays, 1);
    d.tick();  // pause track 2 first
    CHECK_EQ(d.pauses, 1);
    d.over();
    d.tick();  // then track 3
    CHECK_EQ(d.plays, 2);
    CHECK_EQ(d.lastTrack, 3);
    d.started();
    d.over();
    d.tick();
    d.tick();
    CHECK_EQ(d.plays, 2);  // neither 2 nor 3 comes round again
}

TEST(new_play_after_a_loop_ends_on_the_same_frame) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    d.started();
    d.over();          // track 2 ran out; its replay waits for tick()
    d.play(5, false);  // Lua asks for another before that tick
    CHECK_EQ(d.plays, 2);
    CHECK_EQ(d.lastTrack, 5);
    d.tick();
    d.started();
    d.over();
    d.tick();
    CHECK_EQ(d.plays, 2);
}

// Scene load and movie: settleDrive cancels, then stops a playing track itself.
TEST(cancel_drops_a_pending_replay) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    d.started();
    d.over();  // ended just before the scene load
    s.cancel();
    d.tick();
    d.tick();
    CHECK_EQ(d.plays, 1);
}

TEST(cancel_then_stop_while_playing) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    d.started();
    s.cancel();
    CHECK(s.stop());
    d.over();
    d.tick();
    CHECK_EQ(d.plays, 1);
}

TEST(failed_start_leaves_nothing_behind) {
    CDDAState s;
    FakeDrive d{s};
    d.play(2, true);
    d.busy = false;
    s.callback(false);  // no second call after a failure
    d.tick();
    d.tick();
    CHECK_EQ(d.plays, 1);
    CHECK(s.phase() == Phase::Idle);
    CHECK(!s.looping());
}

int main() { return psxsplash::test::runAll(); }
