#include "tilesystem.hh"

#include <psyqo/kernel.hh>

#include "spritesystem.hh"

namespace psxsplash {

// ============================================================================
// On-disk tables (splashpack v23)
// ============================================================================

namespace {

/// Mirrors the exporter's writer byte for byte. Offsets are relative to the
/// start of the splashpack and resolved to pointers at load.
struct SPLASHPACKTilemap {
    uint16_t width;
    uint16_t height;
    uint8_t tileW;
    uint8_t tileH;
    uint8_t tilesetSheet;  // index into the sprite sheet table
    uint8_t pad0;
    uint16_t objectCount;
    uint16_t pad1;
    uint32_t cellsOffset;    // -> width*height TileCell records (2 bytes each)
    uint32_t objectsOffset;  // -> objectCount TileObject records (6 bytes each)
};
static_assert(sizeof(SPLASHPACKTilemap) == 20, "SPLASHPACKTilemap must be 20 bytes");

// The cell and object records ARE the runtime structs: same fields, same order,
// same size, so the runtime points straight into the loaded splashpack instead
// of copying a potentially large grid into its own RAM.
static_assert(sizeof(TileCell) == 2, "TileCell must be 2 bytes to alias the file");
static_assert(sizeof(TileObject) == 6, "TileObject must be 6 bytes to alias the file");

// Screen the map is culled against. The engine renders at 320x240.
constexpr int32_t kScreenW = 320;
constexpr int32_t kScreenH = 240;

}  // namespace

void TileSystem::init() {
    m_cells = nullptr;
    m_objects = nullptr;
    m_objectCount = 0;
    m_width = 0;
    m_height = 0;
    m_tileW = 0;
    m_tileH = 0;
    m_sprites = nullptr;
    m_haveSheet = false;
}

void TileSystem::loadFromSplashpack(uint8_t* data, uint32_t tableOffset, SpriteSystem* sprites) {
    m_cells = nullptr;
    m_objects = nullptr;
    m_objectCount = 0;
    m_haveSheet = false;
    m_sprites = sprites;
    if (tableOffset == 0 || !data) return;

    auto* map = reinterpret_cast<SPLASHPACKTilemap*>(data + tableOffset);
    m_width = map->width;
    m_height = map->height;
    m_tileW = map->tileW;
    m_tileH = map->tileH;

    m_cells = map->cellsOffset ? reinterpret_cast<const TileCell*>(data + map->cellsOffset) : nullptr;

    m_objectCount = map->objectCount <= TILE_MAX_OBJECTS ? map->objectCount : TILE_MAX_OBJECTS;
    m_objects = (map->objectsOffset && m_objectCount > 0)
                    ? reinterpret_cast<const TileObject*>(data + map->objectsOffset)
                    : nullptr;

    // Resolve the tileset once. Without the sheet the map still answers gameplay
    // queries (walkability, objects) - it simply cannot be drawn, which is a far
    // better failure than a crash on a bad sheet index.
    if (m_sprites) {
        const SpriteSheet* s = m_sprites->sheet(map->tilesetSheet);
        if (s) {
            m_sheet = *s;
            m_haveSheet = true;
        }
    }
}

// ============================================================================
// Queries
// ============================================================================

bool TileSystem::walkableAtPixel(int32_t px, int32_t pz) const {
    return tilemath::walkableAtPixel(m_cells, m_width, m_height, m_tileW, m_tileH, px, pz);
}

bool TileSystem::sightClear(int32_t x0, int32_t z0, int32_t x1, int32_t z1,
                            uint16_t maxSteps) const {
    return tilemath::sightClear(m_cells, m_width, m_height, m_tileW, m_tileH, x0, z0, x1, z1,
                                maxSteps);
}

const TileObject* TileSystem::object(int i) const {
    if (!m_objects || i < 0 || i >= m_objectCount) return nullptr;
    return &m_objects[i];
}

void TileSystem::objectCenterPixel(int i, int32_t* px, int32_t* pz) const {
    const TileObject* o = object(i);
    if (!o) {
        if (px) *px = 0;
        if (pz) *pz = 0;
        return;
    }
    tilemath::tileCenterPixel(o->tileX, o->tileY, m_tileW, m_tileH, px, pz);
}

// ============================================================================
// Rendering
// ============================================================================

psyqo::PrimPieces::TPageAttr TileSystem::makeTPage() const {
    psyqo::PrimPieces::TPageAttr attr;
    attr.setPageX(m_sheet.texpageX);
    attr.setPageY(m_sheet.texpageY);
    switch (m_sheet.bitDepth) {
        case 0: attr.set(psyqo::Prim::TPageAttr::Tex4Bits); break;
        case 1: attr.set(psyqo::Prim::TPageAttr::Tex8Bits); break;
        default: attr.set(psyqo::Prim::TPageAttr::Tex16Bits); break;
    }
    attr.setDithering(false);
    return attr;
}

void TileSystem::renderOT(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                          psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc) {
    if (!m_cells || !m_haveSheet || m_tileW == 0 || m_tileH == 0) return;

    int16_t viewX = 0, viewY = 0;
    if (m_sprites) m_sprites->getViewOffset(viewX, viewY);

    // The floor sits at the backmost 2D depth: further back than any sprite
    // layer, in front of the (absent, for a 2D game) 3D world. Larger depth is
    // drawn first, i.e. behind everything inserted at a smaller depth.
    const int depth = Renderer::SPRITE_DEPTH_BASE + Renderer::SPRITE_LAYERS - 1;

    // Only the tiles under the viewport are worth emitting. A 64x64 map is 4096
    // cells; the screen holds at most ~21x16 of them. Cull to that window.
    int32_t x0 = tilemath::pixelToTile(viewX, m_tileW);
    int32_t y0 = tilemath::pixelToTile(viewY, m_tileH);
    int32_t x1 = tilemath::pixelToTile(viewX + kScreenW - 1, m_tileW);
    int32_t y1 = tilemath::pixelToTile(viewY + kScreenH - 1, m_tileH);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)m_width - 1) x1 = (int32_t)m_width - 1;
    if (y1 > (int32_t)m_height - 1) y1 = (int32_t)m_height - 1;

    // clutX is already in 16-pixel units (the exporter's ClutPackingX), so use
    // the two-argument constructor: the Vertex one divides x by 16 again.
    psyqo::PrimPieces::ClutIndex clutIdx(m_sheet.clutX, m_sheet.clutY);

    int emitted = 0;
    for (int32_t ty = y0; ty <= y1; ty++) {
        const TileCell* row = &m_cells[ty * (int)m_width];
        for (int32_t tx = x0; tx <= x1; tx++) {
            const TileCell& c = row[tx];
            if (c.tile == TILE_EMPTY) continue;

            uint8_t u, v;
            spritemath::frameToUV(m_sheet, c.tile, u, v);

            auto& frag = balloc.allocateFragment<psyqo::Prim::Sprite>();
            frag.primitive.position = {.x = (int16_t)(tx * m_tileW - viewX),
                                       .y = (int16_t)(ty * m_tileH - viewY)};
            frag.primitive.size = {.x = (int16_t)m_tileW, .y = (int16_t)m_tileH};
            frag.primitive.setColor({.r = 128, .g = 128, .b = 128});  // neutral, full-bright
            psyqo::PrimPieces::TexInfo texInfo;
            texInfo.u = u;
            texInfo.v = v;
            texInfo.clut = clutIdx;
            frag.primitive.texInfo = texInfo;
            ot.insert(frag, depth);
            emitted++;
        }
    }
    if (emitted == 0) return;

    // One TPage for the whole floor. Inserted last at this depth, so it draws
    // first - the LIFO trick the sprite system and the font both rely on.
    auto& tp = balloc.allocateFragment<psyqo::Prim::TPage>();
    tp.primitive.attr = makeTPage();
    ot.insert(tp, depth);
}

}  // namespace psxsplash
