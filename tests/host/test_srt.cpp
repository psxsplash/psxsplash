// Host tests for a movie's subtitles (src/srt.cpp), compiled natively: what the player keeps from an .SRT and what
// it reports. The array is sized with srt::count, as MoviePlayer::loadSubtitles sizes it, with a canary behind it:
// a block that does not parse must never be written, wherever it is in the file.

#include <string.h>

#include "srt.hh"

#include "testing.hh"

using namespace psxsplash;

namespace {

struct Report {
    unsigned leftOut[8];
    unsigned wide[8];
    unsigned leftOuts = 0, wides = 0;
};

void collect(void* context, unsigned number, srt::Problem problem) {
    Report& r = *static_cast<Report*>(context);
    if (problem == srt::Problem::LeftOut && r.leftOuts < 8) r.leftOut[r.leftOuts++] = number;
    if (problem == srt::Problem::Wide && r.wides < 8) r.wide[r.wides++] = number;
}

struct Parsed {
    srt::Subtitle subs[9];  // room for count(), then the canary
    unsigned room = 0, kept = 0;
    Report report;
};

void parse(const char* text, Parsed& p, unsigned room = 0xffffffff) {
    const size_t size = strlen(text);
    p.room = srt::count(text, size);
    if (room < p.room) p.room = room;
    memset(p.subs, 0x5a, sizeof p.subs);
    p.kept = srt::parse(text, size, p.subs, p.room, collect, &p.report);
}

bool canary(const Parsed& p) {
    const uint8_t* c = reinterpret_cast<const uint8_t*>(&p.subs[p.room]);
    for (size_t i = 0; i < sizeof(srt::Subtitle); i++)
        if (c[i] != 0x5a) return false;
    return true;
}

const char* kGood =
    "1\n00:00:00,500 --> 00:00:02,000\nA line\n\n"
    "2\n00:00:02,500 --> 00:00:04,000\nTwo rows\nof text\n";

}  // namespace

TEST(a_well_formed_file_keeps_every_subtitle) {
    Parsed p;
    parse(kGood, p);
    CHECK_EQ(p.kept, 2);
    CHECK_EQ(p.report.leftOuts, 0);
    CHECK_EQ(p.report.wides, 0);
    CHECK_EQ(p.subs[0].fromMs, 500);
    CHECK_EQ(p.subs[0].toMs, 2000);
    CHECK(strcmp(p.subs[0].text, "A line") == 0);
    CHECK_EQ(p.subs[1].fromMs, 2500);
    CHECK_EQ(p.subs[1].toMs, 4000);
    CHECK(strcmp(p.subs[1].text, "Two rows\nof text") == 0);
    CHECK(canary(p));
}

TEST(crlf_and_a_bom_read_the_same) {
    const char* crlf =
        "\xef\xbb\xbf"
        "1\r\n00:00:00,500 --> 00:00:02,000\r\nA line\r\n\r\n"
        "2\r\n00:00:02,500 --> 00:00:04,000\r\nTwo rows\r\nof text\r\n";
    Parsed a, b;
    parse(kGood, a);
    parse(crlf, b);
    CHECK_EQ(b.kept, 2);
    CHECK_EQ(b.report.leftOuts, 0);
    CHECK(memcmp(a.subs, b.subs, 2 * sizeof(srt::Subtitle)) == 0);
}

TEST(a_broken_time_line_is_left_out_and_named) {
    Parsed p;
    parse("1\n00:00:00,500 --> 00:00:02,000\nA line\n\n"
          "2\n00:00:02,500 -> 00:00:03,000\nNo arrow\n\n"
          "3\n00:00:03,500 --> 00:00:04,000\nLast\n",
          p);
    CHECK_EQ(p.kept, 2);
    CHECK_EQ(p.report.leftOuts, 1);
    CHECK_EQ(p.report.leftOut[0], 2);
    CHECK(strcmp(p.subs[1].text, "Last") == 0);
    CHECK(canary(p));
}

TEST(a_trailing_block_with_no_time_line_writes_nothing) {
    // Every time line in the file is accepted, so count() leaves no spare slot for what follows.
    Parsed p;
    parse("1\n00:00:00,500 --> 00:00:02,000\nA line\n\n"
          "2\n00:00:02,500 --> 00:00:04,000\nAnother\n\n"
          "a stray line\nand its text, which once went past the end of the array\n",
          p);
    CHECK_EQ(p.room, 2);
    CHECK_EQ(p.kept, 2);
    CHECK_EQ(p.report.leftOuts, 1);
    CHECK_EQ(p.report.leftOut[0], 3);
    CHECK(canary(p));
}

TEST(a_trailing_number_alone_is_left_out) {
    Parsed p;
    parse("1\n00:00:00,500 --> 00:00:02,000\nA line\n\n2\n", p);
    CHECK_EQ(p.kept, 1);
    CHECK_EQ(p.report.leftOuts, 1);
    CHECK_EQ(p.report.leftOut[0], 2);
    CHECK(canary(p));
}

TEST(a_subtitle_past_two_rows_of_40_is_kept_and_named) {
    Parsed p;
    parse("1\n00:00:00,500 --> 00:00:02,000\nThis row is a good deal longer than forty characters\n\n"
          "2\n00:00:02,500 --> 00:00:04,000\nOne\nTwo\nThree\n",
          p);
    CHECK_EQ(p.kept, 2);
    CHECK_EQ(p.report.wides, 2);
    CHECK_EQ(p.report.wide[0], 1);
    CHECK_EQ(p.report.wide[1], 2);
    CHECK(strcmp(p.subs[1].text, "One\nTwo\nThree") == 0);
}

TEST(text_longer_than_its_slot_is_cut_and_ended) {
    char text[300] = "1\n00:00:00,500 --> 00:00:02,000\n";
    char* t = text + strlen(text);
    for (int i = 0; i < 3; i++) {
        memset(t, 'a' + i, 39);
        t += 39;
        *t++ = '\n';
    }
    *t = 0;
    Parsed p;
    parse(text, p);
    CHECK_EQ(p.kept, 1);
    CHECK_EQ(strlen(p.subs[0].text), srt::c_text - 1);
    CHECK(canary(p));
}

TEST(no_room_leaves_the_rest_out) {
    Parsed p;
    parse(kGood, p, 1);
    CHECK_EQ(p.kept, 1);
    CHECK_EQ(p.report.leftOuts, 1);
    CHECK_EQ(p.report.leftOut[0], 2);
    CHECK(canary(p));
}

int main() { return psxsplash::test::runAll(); }
