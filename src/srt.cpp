#include "srt.hh"

namespace psxsplash::srt {

namespace {

bool time(const char*& p, const char* end, uint32_t& ms) {
    uint32_t v[4] = {};
    while (p < end && *p == ' ') p++;
    for (int i = 0; i < 4; i++) {
        if (p >= end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') v[i] = v[i] * 10 + uint32_t(*p++ - '0');
        if (i < 3) {
            if (p >= end || (*p != (i < 2 ? ':' : ',') && !(i == 2 && *p == '.'))) return false;
            p++;
        }
    }
    if (v[1] > 59 || v[2] > 59 || v[3] > 999) return false;
    ms = ((v[0] * 60 + v[1]) * 60 + v[2]) * 1000 + v[3];
    return true;
}

bool line(const char*& p, const char* end, const char*& from, const char*& to) {
    if (p >= end) return false;
    from = p;
    while (p < end && *p != '\n') p++;
    to = p;
    if (to > from && to[-1] == '\r') to--;
    if (p < end) p++;
    return true;
}

}  // namespace

unsigned count(const char* data, size_t size) {
    unsigned n = 0;
    for (size_t i = 0; i + 3 <= size; i++) n += data[i] == '-' && data[i + 1] == '-' && data[i + 2] == '>';
    return n;
}

unsigned parse(const char* data, size_t size, Subtitle* out, unsigned max,
               void (*report)(void* context, unsigned number, Problem problem), void* context) {
    const char* s = data;
    const char* end = data + size;
    if (size >= 3 && uint8_t(s[0]) == 0xef && uint8_t(s[1]) == 0xbb && uint8_t(s[2]) == 0xbf) s += 3;
    unsigned kept = 0, number = 0;
    const char *from = nullptr, *to = nullptr;
    while (line(s, end, from, to)) {
        if (from == to) continue;  // the blank lines between blocks
        number++;
        bool digits = true;
        for (const char* c = from; c < to; c++) digits = digits && *c >= '0' && *c <= '9';
        if (digits && !line(s, end, from, to)) {
            report(context, number, Problem::LeftOut);
            break;
        }
        Subtitle sub{};
        const char* p = from;
        bool ok = time(p, to, sub.fromMs);
        while (ok && p < to && *p == ' ') p++;
        ok = ok && to - p >= 3 && p[0] == '-' && p[1] == '-' && p[2] == '>';
        p += 3;
        ok = ok && time(p, to, sub.toMs) && sub.toMs > sub.fromMs;
        size_t used = 0;
        unsigned rows = 0;
        bool wide = false;
        while (line(s, end, from, to) && from != to) {
            if (rows++ && used + 1 < sizeof sub.text) sub.text[used++] = '\n';
            wide = wide || to - from > 40;
            for (const char* c = from; c < to && used + 1 < sizeof sub.text; c++) sub.text[used++] = *c;
        }
        if (!ok || !rows || kept >= max) {
            report(context, number, Problem::LeftOut);
            continue;
        }
        sub.text[used] = 0;
        out[kept++] = sub;
        if (rows > 2 || wide) report(context, number, Problem::Wide);
    }
    return kept;
}

}  // namespace psxsplash::srt
