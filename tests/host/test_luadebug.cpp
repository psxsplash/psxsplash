// Host tests for the Lua debugger's text protocol (src/luadebugmath.hh): the
// breakpoint list and resume command the editor writes over PCdrv, and the
// escaping of the values the engine prints when it stops.

#include "luadebugmath.hh"

#include "testing.hh"

using namespace psxsplash::luadebug;

static int parse(const char* text, Breakpoint* out, int max, uint32_t* gen) {
    return parseBreakpoints(text, (int)strlen(text), out, max, gen);
}

TEST(breakpoints_parse_generation_and_lines) {
    Breakpoint b[kMaxBreakpoints];
    uint32_t gen = 0;
    const int n = parse("7\n12 scripts/door.lua\n3 scripts/a b.lua\r\n", b, kMaxBreakpoints, &gen);
    CHECK_EQ(gen, 7);
    CHECK_EQ(n, 2);
    CHECK_EQ(b[0].line, 12);
    CHECK(!strcmp(b[0].chunk, "scripts/door.lua"));
    CHECK_EQ(b[1].line, 3);
    CHECK(!strcmp(b[1].chunk, "scripts/a b.lua"));  // the rest of the line, CR dropped
}

TEST(breakpoints_skip_bad_lines) {
    Breakpoint b[kMaxBreakpoints];
    uint32_t gen = 0;
    const int n = parse("2\n0 zero.lua\nx.lua\n5\n9 ok.lua\n70000 big.lua\n", b, kMaxBreakpoints, &gen);
    CHECK_EQ(gen, 2);
    CHECK_EQ(n, 1);
    CHECK_EQ(b[0].line, 9);
    // A chunk name longer than kept is skipped, not cut (it could match another file).
    char longName[128] = "1\n4 ";
    for (int i = 0; i < kMaxChunk + 1; i++) strcat(longName, "a");
    CHECK_EQ(parse(longName, b, kMaxBreakpoints, &gen), 0);
}

TEST(breakpoints_empty_file_and_cap) {
    Breakpoint b[kMaxBreakpoints];
    uint32_t gen = 99;
    CHECK_EQ(parse("", b, kMaxBreakpoints, &gen), 0);
    CHECK_EQ(gen, 0);
    CHECK_EQ(parse("3\n1 a\n2 a\n3 a\n", b, 2, &gen), 2);
}

TEST(breakpoint_hits_need_line_and_chunk) {
    Breakpoint b[kMaxBreakpoints];
    uint32_t gen = 0;
    const int n = parse("1\n6 scripts/x.lua\n", b, kMaxBreakpoints, &gen);
    CHECK(lineHasBreakpoint(b, n, 6));
    CHECK(!lineHasBreakpoint(b, n, 7));
    CHECK(hits(b, n, "scripts/x.lua", 6));
    CHECK(!hits(b, n, "scripts/x.lua", 5));
    CHECK(!hits(b, n, "scripts/x.lu", 6));
    CHECK(!hits(b, n, "scripts/x.lua2", 6));
    CHECK(!hits(b, 0, "scripts/x.lua", 6));
}

TEST(command_only_for_its_stop) {
    CHECK(parseCommand("3 continue\n", 11, 3) == Command::Continue);
    CHECK(parseCommand("3 step", 6, 3) == Command::Step);
    CHECK(parseCommand("2 continue\n", 11, 3) == Command::None);  // an earlier stop's
    CHECK(parseCommand("3 stepping", 10, 3) == Command::None);
    CHECK(parseCommand("3", 1, 3) == Command::None);
    CHECK(parseCommand("", 0, 3) == Command::None);
    CHECK(parseCommand("12 step\r\n", 9, 12) == Command::Step);
}

TEST(escape_keeps_one_line) {
    char out[32];
    CHECK_EQ(escape("a\nb\\c\td", 7, out, sizeof(out)), 10);
    CHECK(!strcmp(out, "a\\nb\\\\c\\td"));
    const char ctl[] = {'x', 1, 'y'};
    escape(ctl, 3, out, sizeof(out));
    CHECK(!strcmp(out, "x\\?y"));
}

TEST(escape_cuts_long_values) {
    char out[16];
    const char* s = "0123456789abcdefghijklmnop";
    const int n = escape(s, (int)strlen(s), out, sizeof(out));
    CHECK(n < (int)sizeof(out));
    CHECK_EQ((int)strlen(out), n);
    CHECK(!strcmp(out + n - 3, "..."));
    CHECK(!strncmp(out, "0123456789", 10));
}

int main() { return psxsplash::test::runAll(); }
