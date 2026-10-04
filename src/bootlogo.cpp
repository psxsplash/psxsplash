#include "bootlogo.hh"

#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/sprites.hh>

#include "bootlogo_data.hh"

using namespace psxsplash::bootlogo;

namespace {

// VRAM: the 4bpp page at (640, 0), one CLUT per row at (704, 0). Nothing else
// lives there before the first scene loads.
constexpr int16_t kPageX = 640;
constexpr int16_t kClutX = 704;
constexpr int kPieceCount = 2 * kLetterCount + 1;

// Timeline in milliseconds.
constexpr int kLetterDelay = 130;  // between consecutive letters
constexpr int kDrop = 730;         // drop plus two bounces
constexpr int kPsxStart = 1130;
constexpr int kPsxDrop = 530;
constexpr int kTip = 200;          // PSX tipping onto the slope
constexpr int kEnd = kJingleMs > 2700 ? kJingleMs : 2700;

// Height above the resting position, in pixels, t milliseconds into a drop of
// length d from height h: a fall, then two parabolic bounces.
int bounce(int t, int d, int h) {
    if (t <= 0) return h;
    if (t >= d) return 0;
    int p1 = d * 45 / 100, p2 = d * 75 / 100;
    if (t < p1) {
        int u = t * 1024 / p1;
        return h - h * u / 1024 * u / 1024;
    }
    int a, b, top;
    if (t < p2) {
        a = p1; b = p2; top = h * 18 / 100;
    } else {
        a = p2; b = d; top = h * 5 / 100;
    }
    int u = (t - a) * 1024 / (b - a);
    return top * 4 * u / 1024 * (1024 - u) / 1024;
}

void drawPiece(psyqo::GPU& gpu, const Piece& p, int clut, int16_t dy) {
    psyqo::Prim::Sprite s;
    s.position = {{.x = p.x, .y = int16_t(p.y + dy)}};
    s.size = {{.x = p.w, .y = p.h}};
    s.texInfo.u = p.u;
    s.texInfo.v = p.v;
    s.texInfo.clut = psyqo::PrimPieces::ClutIndex(kClutX >> 4, clut);
    gpu.sendPrimitive(s);
}

}  // namespace

void psxsplash::BootLogo::start(psyqo::GPU& gpu) {
    gpu.uploadToVRAM(kPage, psyqo::Rect{.a = {.x = kPageX, .y = 0}, .b = {64, kPageHeight}});
    gpu.uploadToVRAM(&kCluts[0][0], psyqo::Rect{.a = {.x = kClutX, .y = 0}, .b = {16, kPieceCount}});
    m_audio.init();
    m_audio.loadClip(0, kJingle, sizeof(kJingle), kJingleRate, false);
    m_audio.play(0);
    m_startUs = gpu.now();
}

bool psxsplash::BootLogo::frame(psyqo::GPU& gpu) {
    int ms = int((gpu.now() - m_startUs) / 1000);
    if (ms >= kEnd) {
        m_audio.stopAll();
        return false;
    }

    psyqo::Prim::Rectangle bg({.r = 0x16, .g = 0x12, .b = 0x2b});
    bg.position = {{.x = 0, .y = 0}};
    bg.size = {{.x = 320, .y = 240}};
    gpu.sendPrimitive(bg);

    psyqo::Prim::TPage tpage;
    tpage.attr.setPageX(kPageX >> 6).setPageY(0).set(psyqo::Prim::TPageAttr::Tex4Bits);
    gpu.sendPrimitive(tpage);

    int16_t dy[kLetterCount];
    for (int i = 0; i < kLetterCount; i++) dy[i] = -int16_t(bounce(ms - i * kLetterDelay, kDrop, 220));
    for (int i = 0; i < kLetterCount; i++) drawPiece(gpu, kBack[i], 2 * i, dy[i]);
    for (int i = 0; i < kLetterCount; i++) drawPiece(gpu, kFront[i], 2 * i + 1, dy[i]);

    int t = ms - kPsxStart;
    if (t > 0) {
        int lift = bounce(t, kPsxDrop, 160);
        // Tip angle in 1/4096 turns, eased in and out over kTip.
        int tip = t - kPsxDrop;
        int angle = 0;
        if (tip > 0) {
            int u = tip >= kTip ? 1024 : tip * 1024 / kTip;
            angle = kPsxAngle * (u * u * (3 * 1024 - 2 * u) / 1024 / 1024) / 1024;
        }
        // Small angles only (the rest tilt is ~12 degrees): sin a ~ a, cos a ~ 1 - a^2/2,
        // in 1/4096 fixed point. 2*pi/4096 turns-to-radians is 6.2832/4096.
        int s = angle * 25736 / 4096;  // radians * 4096
        int c = 4096 - s * s / 8192;
        int16_t cx = kPsxCentreX, cy = int16_t(kPsxCentreY - lift);
        auto rot = [&](int x, int y) {
            int rx = x - kPsxCentreX, ry = y - kPsxCentreY;
            return psyqo::Vertex{{.x = int16_t(cx + (rx * c - ry * s) / 4096),
                                  .y = int16_t(cy + (rx * s + ry * c) / 4096)}};
        };
        const Piece& p = kPsx;
        psyqo::Prim::TexturedQuad q;
        q.pointA = rot(p.x, p.y);
        q.pointB = rot(p.x + p.w, p.y);
        q.pointC = rot(p.x, p.y + p.h);
        q.pointD = rot(p.x + p.w, p.y + p.h);
        q.uvA = {.u = p.u, .v = p.v};
        q.uvB = {.u = uint8_t(p.u + p.w), .v = p.v};
        q.uvC = {.u = p.u, .v = uint8_t(p.v + p.h)};
        q.uvD = {.u = uint8_t(p.u + p.w), .v = uint8_t(p.v + p.h)};
        q.clutIndex = psyqo::PrimPieces::ClutIndex(kClutX >> 4, kPieceCount - 1);
        q.tpage.setPageX(kPageX >> 6).setPageY(0).set(psyqo::Prim::TPageAttr::Tex4Bits);
        gpu.sendPrimitive(q);
    }
    return true;
}
