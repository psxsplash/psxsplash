#include "sjis.hh"

namespace psxsplash {

namespace {

// Maps one ASCII character to its fullwidth Shift-JIS code point (big-endian,
// as stored on the card). Letters, digits and space are computed; common
// punctuation is looked up; anything unmapped becomes a fullwidth space.
uint16_t asciiToSjis(char c) {
    if (c == ' ') return 0x8140;
    if (c >= '0' && c <= '9') return 0x824f + (c - '0');
    if (c >= 'A' && c <= 'Z') return 0x8260 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 0x8281 + (c - 'a');
    switch (c) {
        case '!':
            return 0x8149;
        case '"':
            return 0x8168;
        case '#':
            return 0x8194;
        case '$':
            return 0x8190;
        case '%':
            return 0x8193;
        case '&':
            return 0x8195;
        case '\'':
            return 0x8166;
        case '(':
            return 0x8169;
        case ')':
            return 0x816a;
        case '*':
            return 0x8196;
        case '+':
            return 0x817b;
        case ',':
            return 0x8143;
        case '-':
            return 0x817c;
        case '.':
            return 0x8144;
        case '/':
            return 0x815e;
        case ':':
            return 0x8146;
        case ';':
            return 0x8147;
        case '<':
            return 0x8183;
        case '=':
            return 0x8181;
        case '>':
            return 0x8184;
        case '?':
            return 0x8148;
        case '@':
            return 0x8197;
        default:
            return 0x8140;  // fullwidth space for anything unmapped
    }
}

}  // namespace

void asciiToSjisTitle(const char* ascii, uint8_t out[64]) {
    for (int i = 0; i < 64; i++) out[i] = 0;
    if (!ascii) return;

    for (int i = 0; i < 32; i++) {
        char c = ascii[i];
        if (c == '\0') break;
        uint16_t sjis = asciiToSjis(c);
        out[i * 2] = static_cast<uint8_t>(sjis >> 8);
        out[i * 2 + 1] = static_cast<uint8_t>(sjis & 0xff);
    }
}

}  // namespace psxsplash
