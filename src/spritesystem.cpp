#include "spritesystem.hh"

#include <psyqo/kernel.hh>

#include "gameobject.hh"
#include "streq.hh"

namespace psxsplash {

// ============================================================================
// On-disk tables (splashpack v22)
// ============================================================================

namespace {

/// Mirrors the exporter's writer byte for byte. Name offsets are relative to the
/// start of the splashpack, resolved to pointers at load.
struct SPLASHPACKSpriteSheet {
    uint32_t nameOffset;
    uint8_t texpageX, texpageY;
    uint8_t u0, v0;
    uint16_t clutX, clutY;
    uint8_t cellW, cellH;
    uint8_t cols, rows;
    uint8_t bitDepth;
    uint8_t pad0;
    uint16_t pad1;
};
static_assert(sizeof(SPLASHPACKSpriteSheet) == 20, "SPLASHPACKSpriteSheet must be 20 bytes");

struct SPLASHPACKSpriteAnim {
    uint32_t nameOffset;
    uint8_t sheet;
    uint8_t firstFrame;
    uint8_t frameCount;
    uint8_t frameDuration;
    uint8_t loop;
    uint8_t pad0;
    uint16_t pad1;
};
static_assert(sizeof(SPLASHPACKSpriteAnim) == 12, "SPLASHPACKSpriteAnim must be 12 bytes");

}  // namespace

// The pools are statically allocated inside SceneManager, so their cost is paid
// on every scene whether or not it uses a single sprite. Pin it: on a 2 MB
// console an accidental widening of SpriteInstance is worth noticing here rather
// than in a link failure months later.
static_assert(sizeof(SpriteInstance) <= 48, "SpriteInstance grew; re-check the RAM budget");
static_assert(sizeof(SpriteInstance) * SPRITE_MAX + sizeof(SpriteSheet) * SPRITE_MAX_SHEETS +
                      sizeof(SpriteAnim) * SPRITE_MAX_ANIMS <=
                  8 * 1024,
              "Sprite pools exceed their 8 KB budget");

// ============================================================================
// Lifecycle
// ============================================================================

void SpriteSystem::init() {
    for (int i = 0; i < SPRITE_MAX; i++) m_sprites[i].active = false;
    m_sheetCount = 0;
    m_animCount = 0;
    m_spriteCount = 0;
    m_viewX = 0;
    m_viewY = 0;
}

void SpriteSystem::loadFromSplashpack(uint8_t* data, uint16_t sheetCount, uint16_t animCount,
                                      uint32_t tableOffset) {
    m_sheetCount = 0;
    m_animCount = 0;
    if (tableOffset == 0) return;

    psyqo::Kernel::assert(sheetCount <= SPRITE_MAX_SHEETS, "Splashpack has too many sprite sheets");
    psyqo::Kernel::assert(animCount <= SPRITE_MAX_ANIMS, "Splashpack has too many sprite anims");

    uint8_t* cursor = data + tableOffset;

    for (uint16_t i = 0; i < sheetCount; i++) {
        auto* d = reinterpret_cast<SPLASHPACKSpriteSheet*>(cursor);
        SpriteSheet& s = m_sheets[m_sheetCount++];
        s.name = d->nameOffset ? reinterpret_cast<const char*>(data + d->nameOffset) : nullptr;
        s.texpageX = d->texpageX;
        s.texpageY = d->texpageY;
        s.clutX = d->clutX;
        s.clutY = d->clutY;
        s.u0 = d->u0;
        s.v0 = d->v0;
        s.cellW = d->cellW;
        s.cellH = d->cellH;
        s.cols = d->cols;
        s.rows = d->rows;
        s.bitDepth = d->bitDepth;
        cursor += sizeof(SPLASHPACKSpriteSheet);
    }

    for (uint16_t i = 0; i < animCount; i++) {
        auto* d = reinterpret_cast<SPLASHPACKSpriteAnim*>(cursor);
        SpriteAnim& a = m_anims[m_animCount++];
        a.name = d->nameOffset ? reinterpret_cast<const char*>(data + d->nameOffset) : nullptr;
        a.sheet = d->sheet;
        a.firstFrame = d->firstFrame;
        a.frameCount = d->frameCount;
        a.frameDuration = d->frameDuration;
        a.loop = d->loop != 0;
        cursor += sizeof(SPLASHPACKSpriteAnim);
    }
}

void SpriteSystem::relocate(intptr_t delta) {
    for (int i = 0; i < m_sheetCount; i++)
        if (m_sheets[i].name) m_sheets[i].name += delta;
    for (int i = 0; i < m_animCount; i++)
        if (m_anims[i].name) m_anims[i].name += delta;
}

// ============================================================================
// Lookup
// ============================================================================

int SpriteSystem::sheetIndex(const char* name) const {
    if (!name) return -1;
    for (int i = 0; i < m_sheetCount; i++)
        if (m_sheets[i].name && streq(m_sheets[i].name, name)) return i;
    return -1;
}

int SpriteSystem::animIndex(const char* name) const {
    if (!name) return -1;
    for (int i = 0; i < m_animCount; i++)
        if (m_anims[i].name && streq(m_anims[i].name, name)) return i;
    return -1;
}

const SpriteAnim* SpriteSystem::anim(int i) const {
    if (i < 0 || i >= m_animCount) return nullptr;
    return &m_anims[i];
}

const SpriteSheet* SpriteSystem::sheet(int i) const {
    if (i < 0 || i >= m_sheetCount) return nullptr;
    return &m_sheets[i];
}

// ============================================================================
// Instances
// ============================================================================

int SpriteSystem::create(int sheet) {
    if (sheet < 0 || sheet >= m_sheetCount) return -1;
    for (int i = 0; i < SPRITE_MAX; i++) {
        if (m_sprites[i].active) continue;
        SpriteInstance& s = m_sprites[i];
        s = {};
        s.active = true;
        s.visible = true;
        s.plane = SpritePlane::Screen;
        s.sheet = (uint8_t)sheet;
        s.color = {.r = 128, .g = 128, .b = 128};  // neutral tint
        s.anim = -1;
        m_spriteCount++;
        return i;
    }
    return -1;
}

void SpriteSystem::destroy(int id) {
    if (!valid(id)) return;
    m_sprites[id].active = false;
    m_sprites[id].boundActor = nullptr;
    m_spriteCount--;
}

void SpriteSystem::setPos(int id, int16_t x, int16_t y) {
    if (!valid(id)) return;
    m_sprites[id].x = x;
    m_sprites[id].y = y;
}

void SpriteSystem::setWorldPos(int id, const psyqo::Vec3& p) {
    if (!valid(id)) return;
    m_sprites[id].worldPos = p;
}

void SpriteSystem::bindToActor(int id, GameObject* actor, SpritePlane plane, int16_t offX,
                               int16_t offY) {
    if (!valid(id)) return;
    m_sprites[id].boundActor = actor;
    m_sprites[id].plane = plane;
    m_sprites[id].boundOffsetX = offX;
    m_sprites[id].boundOffsetY = offY;
}

void SpriteSystem::setFrame(int id, uint8_t frame) {
    if (!valid(id)) return;
    m_sprites[id].frame = frame;
    m_sprites[id].anim = -1;  // an explicit frame wins over whatever was playing
    m_sprites[id].animPlaying = false;
}

void SpriteSystem::playAnim(int id, int anim, bool restart) {
    if (!valid(id)) return;
    if (anim < 0 || anim >= m_animCount) return;
    SpriteInstance& s = m_sprites[id];
    if (s.anim == anim && s.animPlaying && !restart) return;  // don't stutter

    const SpriteAnim& a = m_anims[anim];
    s.anim = (int8_t)anim;
    s.sheet = a.sheet;
    s.animFrame = 0;
    s.animTimer = 0;
    s.animAccum = 0;
    s.animPlaying = true;
    s.frame = a.firstFrame;
}

void SpriteSystem::stopAnim(int id) {
    if (!valid(id)) return;
    m_sprites[id].animPlaying = false;
}

void SpriteSystem::setFacingFromYaw(int id, int animBase, uint8_t dirCount, int32_t yawRaw) {
    if (!valid(id)) return;
    if (dirCount == 0) return;
    const int target = animBase + spritemath::facingFromYaw(yawRaw, dirCount);
    if (target < 0 || target >= m_animCount) return;

    SpriteInstance& s = m_sprites[id];
    if (s.anim == target) return;

    // Carry the cadence across the switch. Restarting at frame 0 every time the
    // player crosses a sector boundary is what makes a turning character stutter.
    const uint8_t keepFrame = s.animFrame;
    const uint8_t keepTimer = s.animTimer;
    const bool wasPlaying = s.animPlaying;
    playAnim(id, target, true);
    const SpriteAnim& a = m_anims[target];
    s.animFrame = keepFrame < a.frameCount ? keepFrame : 0;
    s.animTimer = keepTimer;
    s.animPlaying = wasPlaying;
    s.frame = (uint8_t)(a.firstFrame + s.animFrame);
}

void SpriteSystem::setVisible(int id, bool v) {
    if (!valid(id)) return;
    m_sprites[id].visible = v;
}

bool SpriteSystem::isVisible(int id) const {
    if (!valid(id)) return false;
    return m_sprites[id].visible;
}

void SpriteSystem::setFlip(int id, bool fx, bool fy) {
    if (!valid(id)) return;
    m_sprites[id].flipX = fx;
    m_sprites[id].flipY = fy;
}

void SpriteSystem::setColor(int id, uint8_t r, uint8_t g, uint8_t b) {
    if (!valid(id)) return;
    m_sprites[id].color = {.r = r, .g = g, .b = b};
}

void SpriteSystem::setLayer(int id, uint8_t layer) {
    if (!valid(id)) return;
    m_sprites[id].layer = layer < Renderer::SPRITE_LAYERS ? layer : Renderer::SPRITE_LAYERS - 1;
}

void SpriteSystem::setSize(int id, int16_t w, int16_t h) {
    if (!valid(id)) return;
    m_sprites[id].w = w;
    m_sprites[id].h = h;
}

void SpriteSystem::setIgnoreViewOffset(int id, bool ignore) {
    if (!valid(id)) return;
    m_sprites[id].ignoreViewOffset = ignore;
}

void SpriteSystem::setViewOffset(int16_t x, int16_t y) {
    m_viewX = x;
    m_viewY = y;
}

void SpriteSystem::getViewOffset(int16_t& x, int16_t& y) const {
    x = m_viewX;
    y = m_viewY;
}

// ============================================================================
// Per-frame update
// ============================================================================

void SpriteSystem::update(int32_t dt12) {
    // Animations are authored in VSYNC frames (a frameDuration of 6 means six
    // 60Hz frames per cell), and dt12 measures real time with 4096 == one 30Hz
    // frame - so one vsync frame is 2048 raw units.
    //
    // The accumulator is what makes a walk cycle play at the same speed however
    // the frame rate wanders. Stepping once per update() instead meant every
    // character animated in lockstep with the renderer: slow down and everyone
    // moon-walks. Same sub-frame carry pattern as AnimationPlayer.
    constexpr int32_t c_vsyncDt = 2048;

    for (int i = 0; i < SPRITE_MAX; i++) {
        SpriteInstance& s = m_sprites[i];
        if (!s.active) continue;

        if (s.animPlaying && s.anim >= 0) {
            const SpriteAnim& a = m_anims[s.anim];
            s.animAccum += dt12;
            // Bounded: SceneManager clamps dt12 to 4 frames, so this runs at most
            // 8 times. The carry keeps fractional time rather than discarding it.
            while (s.animAccum >= c_vsyncDt && s.animPlaying) {
                s.animAccum -= c_vsyncDt;
                auto tick = spritemath::advanceAnim(s.animFrame, s.animTimer, a.frameCount,
                                                    a.frameDuration, a.loop);
                s.animFrame = tick.frame;
                s.animTimer = tick.timer;
                s.frame = (uint8_t)(a.firstFrame + tick.frame);
                if (tick.finished) s.animPlaying = false;
            }
        }

        if (s.boundActor) {
            if (s.plane == SpritePlane::World) {
                s.worldPos = s.boundActor->position;
            } else {
                // Screen plane: the actor's X/Z is the 2D ground plane, which is
                // what a top-down 2D game moves on. Y (height) is deliberately
                // ignored - a jumping actor should not slide up the screen.
                s.x = (int16_t)(s.boundActor->position.x.integer() + s.boundOffsetX);
                s.y = (int16_t)(s.boundActor->position.z.integer() + s.boundOffsetY);
            }
        }
    }
}

// ============================================================================
// Rendering
// ============================================================================

psyqo::PrimPieces::TPageAttr SpriteSystem::makeTPage(const SpriteSheet& sheet) {
    psyqo::PrimPieces::TPageAttr attr;
    attr.setPageX(sheet.texpageX);
    attr.setPageY(sheet.texpageY);
    switch (sheet.bitDepth) {
        case 0: attr.set(psyqo::Prim::TPageAttr::Tex4Bits); break;
        case 1: attr.set(psyqo::Prim::TPageAttr::Tex8Bits); break;
        default: attr.set(psyqo::Prim::TPageAttr::Tex16Bits); break;
    }
    attr.setDithering(false);
    return attr;
}

void SpriteSystem::renderSprite(SpriteInstance& s, int depth,
                                Renderer::OT& ot,
                                Renderer::Balloc& balloc) {
    const SpriteSheet& sheet = m_sheets[s.sheet];

    uint8_t u, v;
    spritemath::frameToUV(sheet, s.frame, u, v);

    int16_t x = s.x;
    int16_t y = s.y;
    if (!s.ignoreViewOffset) {
        x -= m_viewX;
        y -= m_viewY;
    }

    const int16_t w = s.w ? s.w : (int16_t)sheet.cellW;
    const int16_t h = s.h ? s.h : (int16_t)sheet.cellH;

    // clutX is already in 16-pixel units (the exporter's ClutPackingX), so use
    // the two-argument constructor: the Vertex one divides x by 16 again.
    psyqo::PrimPieces::ClutIndex clutIdx(sheet.clutX, sheet.clutY);

    // Prim::Sprite is a 1:1 blit: it cannot scale and it cannot flip. When the
    // sprite asks for either, fall back to a quad, which can do both and carries
    // its own TPage so it needs no separate control primitive.
    const bool fastPath = !s.flipX && !s.flipY && w == (int16_t)sheet.cellW &&
                          h == (int16_t)sheet.cellH;

    if (fastPath) {
        auto& frag = balloc.allocateFragment<psyqo::Prim::Sprite>();
        frag.primitive.position = {.x = x, .y = y};
        frag.primitive.size = {.x = w, .y = h};
        frag.primitive.setColor(s.color);
        psyqo::PrimPieces::TexInfo texInfo;
        texInfo.u = u;
        texInfo.v = v;
        texInfo.clut = clutIdx;
        frag.primitive.texInfo = texInfo;
        ot.insert(frag, depth);
        return;
    }

    const uint8_t uL = s.flipX ? (uint8_t)(u + sheet.cellW - 1) : u;
    const uint8_t uR = s.flipX ? u : (uint8_t)(u + sheet.cellW - 1);
    const uint8_t vT = s.flipY ? (uint8_t)(v + sheet.cellH - 1) : v;
    const uint8_t vB = s.flipY ? v : (uint8_t)(v + sheet.cellH - 1);

    auto& q = balloc.allocateFragment<psyqo::Prim::TexturedQuad>();
    q.primitive.setColor(s.color);
    q.primitive.pointA = {{.x = x, .y = y}};
    q.primitive.pointB = {{.x = (int16_t)(x + w), .y = y}};
    q.primitive.pointC = {{.x = x, .y = (int16_t)(y + h)}};
    q.primitive.pointD = {{.x = (int16_t)(x + w), .y = (int16_t)(y + h)}};
    q.primitive.uvA.u = uL;
    q.primitive.uvA.v = vT;
    q.primitive.uvB.u = uR;
    q.primitive.uvB.v = vT;
    q.primitive.uvC.u = uL;
    q.primitive.uvC.v = vB;
    q.primitive.uvD.u = uR;
    q.primitive.uvD.v = vB;
    q.primitive.clutIndex = clutIdx;
    q.primitive.tpage = makeTPage(sheet);
    ot.insert(q, depth);
}

void SpriteSystem::renderOT(Renderer::OT& ot,
                            Renderer::Balloc& balloc) {
    if (m_spriteCount == 0) return;

    // Sprites still have to be emitted grouped by (layer, sheet), so each sheet
    // contributes one TPage per layer instead of one per sprite. But the old way
    // of achieving that was to re-scan all 128 slots once per (layer, sheet)
    // pair: SPRITE_LAYERS(8) x sheets(4) x 128 = 4096 iterations every frame to
    // emit ~30 sprites, i.e. most of a millisecond spent on rejection tests
    // alone. It cost the same whether the scene had two sprites or a hundred.
    //
    // Instead: ONE pass over the pool to bucket the drawable sprites by
    // (layer, sheet), then walk the buckets. That is 128 + N rather than
    // 4096, and it keeps the emission order identical - the counting sort below
    // is stable, so within a bucket ids still come out ascending exactly as the
    // nested loops produced them.
    const int buckets = Renderer::SPRITE_LAYERS * m_sheetCount;
    if (buckets <= 0) return;

    uint8_t counts[Renderer::SPRITE_LAYERS * SPRITE_MAX_SHEETS] = {};
    uint8_t order[SPRITE_MAX];
    int drawable = 0;

    for (int i = 0; i < SPRITE_MAX; i++) {
        const SpriteInstance& s = m_sprites[i];
        if (!s.active || !s.visible) continue;
        if (s.plane != SpritePlane::Screen) continue;  // World: not drawn yet
        if (s.layer < 0 || s.layer >= Renderer::SPRITE_LAYERS) continue;
        if (s.sheet < 0 || s.sheet >= m_sheetCount) continue;
        counts[s.layer * m_sheetCount + s.sheet]++;
        drawable++;
    }
    if (drawable == 0) return;

    // Prefix sum -> where each bucket starts in `order`.
    int starts[Renderer::SPRITE_LAYERS * SPRITE_MAX_SHEETS];
    int cursor[Renderer::SPRITE_LAYERS * SPRITE_MAX_SHEETS];
    int running = 0;
    for (int b = 0; b < buckets; b++) {
        starts[b] = running;
        cursor[b] = running;
        running += counts[b];
    }

    for (int i = 0; i < SPRITE_MAX; i++) {
        const SpriteInstance& s = m_sprites[i];
        if (!s.active || !s.visible) continue;
        if (s.plane != SpritePlane::Screen) continue;
        if (s.layer < 0 || s.layer >= Renderer::SPRITE_LAYERS) continue;
        if (s.sheet < 0 || s.sheet >= m_sheetCount) continue;
        order[cursor[s.layer * m_sheetCount + s.sheet]++] = (uint8_t)i;
    }

    for (int layer = 0; layer < Renderer::SPRITE_LAYERS; layer++) {
        const int depth = Renderer::SPRITE_DEPTH_BASE + layer;

        for (int sheetIdx = 0; sheetIdx < m_sheetCount; sheetIdx++) {
            const int b = layer * m_sheetCount + sheetIdx;
            if (counts[b] == 0) continue;

            for (int k = starts[b]; k < starts[b] + counts[b]; k++) {
                renderSprite(m_sprites[order[k]], depth, ot, balloc);
            }

            // Inserted last at this depth, so it is drawn FIRST - the same LIFO
            // trick the proportional font uses to scope a TPage to its own batch.
            auto& tp = balloc.allocateFragment<psyqo::Prim::TPage>();
            tp.primitive.attr = makeTPage(m_sheets[sheetIdx]);
            ot.insert(tp, depth);
        }
    }
}

}  // namespace psxsplash
