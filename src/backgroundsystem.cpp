#include "backgroundsystem.hh"

#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/sprites.hh>

#include "camera.hh"

namespace psxsplash {

void BackgroundSystem::init() {
    m_pack = nullptr;
    m_table = 0;
    m_objects = nullptr;
    m_current = BACKGROUND_NONE;
}

void BackgroundSystem::load(const uint8_t* pack, uint32_t tableOffset, eastl::vector<GameObject*>* objects) {
    m_pack = pack;
    m_table = tableOffset;
    m_objects = objects;
    m_current = BACKGROUND_NONE;
    // The bit is runtime state: whatever the pack holds there starts clear.
    if (objects)
        for (GameObject* o : *objects) o->setBackgroundHidden(false);
}

const char* BackgroundSystem::name(int i) const {
    const BackgroundRecord* r = backgroundmath::record(m_pack, m_table, i);
    return (r && r->nameOffset) ? reinterpret_cast<const char*>(m_pack + r->nameOffset) : nullptr;
}

void BackgroundSystem::setHidden(int i, bool hidden) {
    const BackgroundRecord* r = backgroundmath::record(m_pack, m_table, i);
    if (!r || !m_objects || r->hiddenCount == 0) return;
    const uint16_t* idx = reinterpret_cast<const uint16_t*>(m_pack + r->hiddenOffset);
    for (uint16_t k = 0; k < r->hiddenCount; k++)
        if (idx[k] < m_objects->size()) (*m_objects)[idx[k]]->setBackgroundHidden(hidden);
}

bool BackgroundSystem::show(int i, Camera& camera) {
    const BackgroundRecord* r = backgroundmath::record(m_pack, m_table, i);
    if (!r) return false;
    if (m_current != BACKGROUND_NONE) setHidden(m_current, false);
    m_current = i;
    setHidden(i, true);
    camera.SetPosition(psyqo::FixedPoint<12>(r->camX, psyqo::FixedPoint<12>::RAW),
                       psyqo::FixedPoint<12>(r->camY, psyqo::FixedPoint<12>::RAW),
                       psyqo::FixedPoint<12>(r->camZ, psyqo::FixedPoint<12>::RAW));
    psyqo::Matrix33 m;
    for (int row = 0; row < 3; row++) {
        m.vs[row].x = psyqo::FixedPoint<12>(int32_t(r->rot[row * 3 + 0]), psyqo::FixedPoint<12>::RAW);
        m.vs[row].y = psyqo::FixedPoint<12>(int32_t(r->rot[row * 3 + 1]), psyqo::FixedPoint<12>::RAW);
        m.vs[row].z = psyqo::FixedPoint<12>(int32_t(r->rot[row * 3 + 2]), psyqo::FixedPoint<12>::RAW);
    }
    camera.SetRotationMatrix(m, r->angleX, r->angleY, r->angleZ);
    if (r->h > 0) camera.SetProjectionH(r->h);
    return true;
}

void BackgroundSystem::hide() {
    if (m_current == BACKGROUND_NONE) return;
    setHidden(m_current, false);
    m_current = BACKGROUND_NONE;
}

// A tile is a sprite after its own TPage (sprites take the page from the
// GPU state). Within a bucket the last insert draws first, so the TPage goes
// in after its sprite. Dithering stays on as the frame's first TPage set it:
// untextured triangles drawn later read it from the same state.
void BackgroundSystem::insertTile(const BackgroundTile& t, int32_t bucket, Renderer::OT& ot, Renderer::Balloc& balloc) {
    {
        auto& s = balloc.allocateFragment<psyqo::Prim::Sprite>();
        s.primitive.position = {.x = t.x, .y = t.y};
        s.primitive.size = {.x = (int16_t)t.w, .y = (int16_t)t.h};
        s.primitive.setColor({.r = 128, .g = 128, .b = 128});
        psyqo::PrimPieces::TexInfo ti;
        ti.u = t.u;
        ti.v = t.v;
        ti.clut = psyqo::PrimPieces::ClutIndex(t.clutX, t.clutY);
        s.primitive.texInfo = ti;
        ot.insert(s, bucket);
        auto& tp = balloc.allocateFragment<psyqo::Prim::TPage>();
        tp.primitive.attr.setPageX(t.texpageX);
        tp.primitive.attr.setPageY(t.texpageY);
        switch (t.bitDepth) {
            case 0: tp.primitive.attr.set(psyqo::Prim::TPageAttr::Tex4Bits); break;
            case 1: tp.primitive.attr.set(psyqo::Prim::TPageAttr::Tex8Bits); break;
            default: tp.primitive.attr.set(psyqo::Prim::TPageAttr::Tex16Bits); break;
        }
        tp.primitive.attr.setDithering(true);
        ot.insert(tp, bucket);
    }
}

void BackgroundSystem::renderOT(Renderer::OT& ot, Renderer::Balloc& balloc, uint32_t otSize) {
    const BackgroundRecord* r = backgroundmath::record(m_pack, m_table, m_current);
    if (!r) return;
    // Layer tiles first: one clamped onto the image bucket must still draw
    // after the image, and the image, inserted last, draws first.
    const BackgroundTile* layer = reinterpret_cast<const BackgroundTile*>(m_pack + r->layerTilesOffset);
    for (uint16_t i = 0; i < r->layerTileCount; i++)
        insertTile(layer[i], backgroundmath::layerBucket(layer[i].nearSZ, otSize, Renderer::WORLD_DEPTH_MIN), ot, balloc);
    const BackgroundTile* image = reinterpret_cast<const BackgroundTile*>(m_pack + r->tilesOffset);
    const int32_t back = backgroundmath::imageBucket(otSize);
    for (uint16_t i = 0; i < r->tileCount; i++) insertTile(image[i], back, ot, balloc);
}

}  // namespace psxsplash
