#include "renderer.hh"

#include "fade.hh"
#include "palettecycler.hh"

#include <EASTL/array.h>
#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>
#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/kernel.hh>
#include <psyqo/matrix.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/gte-math.hh>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>

#include "common/syscalls/syscalls.h"

#include "gtemath.hh"
#if PSXSPLASH_FEATURE_LIGHTS
#include "lightgte.hh"
#endif
#include "skinmesh.hh"
#include "spritesystem.hh"
#include "tilesystem.hh"
#include "uisystem.hh"
#ifdef PSXSPLASH_MEMOVERLAY
#include "memoverlay.hh"
#endif
#ifdef PSXSPLASH_PROFILER
#include "profiler.hh"
#endif

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;
using namespace psyqo::GTE;

// Development builds count the triangles the renderer loses or bends
// (m_lost, printed by reportPeaks); other builds compile the counting out.
#ifdef PCDRV_SUPPORT
#define RENDER_LOST(...) __VA_ARGS__
#else
#define RENDER_LOST(...)
#endif

psxsplash::Renderer* psxsplash::Renderer::instance = nullptr;

void psxsplash::Renderer::Init(psyqo::GPU& gpuInstance) {
    psyqo::Kernel::assert(instance == nullptr,
                          "A second initialization of Renderer was tried");
    clear<Register::TRX, Safe>();
    clear<Register::TRY, Safe>();
    clear<Register::TRZ, Safe>();
    write<Register::OFX, Safe>(psyqo::FixedPoint<16>(160.0).raw());
    write<Register::OFY, Safe>(psyqo::FixedPoint<16>(120.0).raw());
    write<Register::H, Safe>(PROJ_H);
    if (!instance) { instance = new Renderer(gpuInstance); }
}

void psxsplash::Renderer::Configure(uint32_t otSize, uint32_t bumpSize) {
    if (otSize == 0) otSize = DEFAULT_ORDERING_TABLE_SIZE;
    if (bumpSize == 0) bumpSize = DEFAULT_BUMP_ALLOCATOR_SIZE;
    if (m_otSize != 0) {
        psyqo::Kernel::assert(otSize <= m_otSize && bumpSize <= m_bumpSize,
                              "Scene needs larger render buffers than the first one: re-export the whole project");
        return;
    }
    m_otSize = otSize;
    m_bumpSize = (bumpSize + 3) & ~3u;
    for (int i = 0; i < 2; i++) {
        m_ots[i] = new OT(new psyqo::Fragments::ChainEntry[m_otSize + 1], m_otSize);
        m_ballocs[i] = new Balloc(new uint32_t[m_bumpSize / 4], m_bumpSize);
    }
    write<Register::ZSF3, Safe>(m_otSize / 3);
    write<Register::ZSF4, Safe>(m_otSize / 4);
}

void psxsplash::Renderer::ReserveVisibleTriangles(uint32_t count) {
    if (count <= m_visibleCapacity) return;
    delete[] m_visibleRefs;
    m_visibleRefs = new TriangleRef[count];
    m_visibleCapacity = count;
}

void psxsplash::Renderer::reportPeaks() {
#ifdef PCDRV_SUPPORT
    // Development builds tell the editor what a scene really used, at most
    // once a second and only when a peak grew.
    if (m_frameCount % 60 != 0) return;
    // And how many triangles it lost or bent over that second, when any.
    // The clip, split, threshold and refused fields go last, so parsers of the fields before them keep working.
    if (m_lost.ot || m_lost.skinNear || m_lost.leaf || m_lost.clamp || m_lost.clip || m_lost.split || m_lost.refused)
        ramsyscall_printf(
            "psxsplash: render lost ot %u skinnear %u leaf %u clamp %u of %u tris clip %u split %u eq %u %u refused %u\n",
            m_lost.ot, m_lost.skinNear, m_lost.leaf, m_lost.clamp, m_lost.tris, m_lost.clip, m_lost.split, m_lost.eqLo,
            m_lost.eqHi, m_lost.refused);
    m_lost = LostCounts();
    if (m_peakZ == m_reportedZ && m_peakBump == m_reportedBump) return;
    m_reportedZ = m_peakZ;
    m_reportedBump = m_peakBump;
    ramsyscall_printf("psxsplash: render peak depth %d of %u, bump %u of %u\n", (int)m_peakZ, m_otSize,
                      m_peakBump, m_bumpSize);
#endif
}

void psxsplash::Renderer::SetCamera(psxsplash::Camera& camera) {
    m_currentCamera = &camera;
    // Update GTE H register to match camera's projection distance
    setProjectionH(camera.GetProjectionH());
}

void psxsplash::Renderer::setProjectionH(int32_t h) {
    write<Register::H, Unsafe>(h);
    m_projH = h;
    m_splitZ = (h >> 1) * SPLIT_NEAR;
    m_nearSZ = (h >> 1) + 1;
}

void psxsplash::Renderer::SetFog(const FogConfig& fog) {
    m_fog = fog;
    // Always use fog color as the GPU clear/back color
    m_clearcolor = fog.color;
    if (fog.enabled) {
        m_fog.fogFarSZ = 20000 / fog.density;
    } else {
        m_fog.fogFarSZ = 0;
    }
}

psyqo::Vec3 psxsplash::Renderer::computeCameraViewPos() {
    ::clear<Register::TRX, Safe>();
    ::clear<Register::TRY, Safe>();
    ::clear<Register::TRZ, Safe>();
    writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
    writeSafe<PseudoRegister::V0>(-m_currentCamera->GetPosition());
    Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
    return readSafe<PseudoRegister::SV>();
}

void psxsplash::Renderer::setupObjectTransform(
    GameObject* obj, const psyqo::Vec3& cameraPosition) {
    ::clear<Register::TRX, Safe>();
    ::clear<Register::TRY, Safe>();
    ::clear<Register::TRZ, Safe>();
    writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
    writeSafe<PseudoRegister::V0>(obj->position);
    Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
    psyqo::Vec3 objectPosition = readSafe<PseudoRegister::SV>();
    objectPosition.x += cameraPosition.x;
    objectPosition.y += cameraPosition.y;
    objectPosition.z += cameraPosition.z;
    psyqo::Matrix33 finalMatrix;
    // Arguments are reversed against the old MatrixMultiplyGTE: that one loaded
    // its FIRST argument into RT and so computed matA * matB, while GteMath
    // computes out = m2 * m1. Same clobber set (RT, V0, IR1-3, MAC1-3), and RT
    // is rewritten immediately below either way.
    psyqo::GteMath::multiplyMatrix33(obj->rotation, m_currentCamera->GetRotation(), &finalMatrix);
    writeSafe<PseudoRegister::Translation>(objectPosition);
    writeSafe<PseudoRegister::Rotation>(finalMatrix);

#if PSXSPLASH_FEATURE_LIGHTS
    m_objLights.count = 0;
    if (m_lightCount > 0 && obj->isDynamicLit()) prepareObjectLights(obj);
#endif
}

#if PSXSPLASH_FEATURE_LIGHTS
void psxsplash::Renderer::prepareObjectLights(const GameObject* obj) {
    const int32_t pos[3] = {obj->position.x.raw(), obj->position.y.raw(), obj->position.z.raw()};
    int32_t rot[3][3];
    for (int i = 0; i < 3; i++) {
        rot[i][0] = obj->rotation.vs[i].x.raw();
        rot[i][1] = obj->rotation.vs[i].y.raw();
        rot[i][2] = obj->rotation.vs[i].z.raw();
    }
    const int32_t aabbMin[3] = {obj->aabbMinX, obj->aabbMinY, obj->aabbMinZ};
    const int32_t aabbMax[3] = {obj->aabbMaxX, obj->aabbMaxY, obj->aabbMaxZ};
    m_objLightsSmooth = obj->isDynamicLitSmooth();
    if (lightmath::prepare(m_lights, m_lightCount, pos, rot, aabbMin, aabbMax, m_objLights) == 0) return;
    if (m_objLightsSmooth) {
        loadLightMatrices(m_objLights);
    } else {
        for (int i = 0; i < m_objLights.count; i++)
            m_objLights.lights[i].colourLut = colourLutFor(m_objLights.lights[i].sceneIndex);
        loadFlatLightState(m_objLights);
    }
}

const uint32_t* psxsplash::Renderer::colourLutFor(int sceneIndex) {
    const PointLight& l = m_lights[sceneIndex];
    ColourLut& lut = m_colourLuts[sceneIndex];
    const uint32_t key = (uint32_t)l.r | ((uint32_t)l.g << 8) | ((uint32_t)l.b << 16);
    if (lut.key != key || lut.intensity != l.intensity) {
        lut.key = key;
        lut.intensity = l.intensity;
        const int32_t cr = (l.r * l.intensity) >> 12, cg = (l.g * l.intensity) >> 12,
                      cb = (l.b * l.intensity) >> 12;
        for (int i = 0; i < lightmath::kColourLutSize; i++)
            lut.rgb[i] = lightmath::flatColour(cr, cg, cb, i << lightmath::kColourLutShift);
    }
    return lut.rgb;
}
#endif

// Per-vertex fog blend for untextured triangles: interpolate vertex color toward fog color.
static inline psyqo::Color fogBlend(psyqo::Color vc, int32_t ir0, psyqo::Color fogC) {
    if (ir0 <= 0) return vc;
    if (ir0 >= 4096) return fogC;
    int32_t keep = 4096 - ir0;
    return {
        .r = (uint8_t)((vc.r * keep + fogC.r * ir0) >> 12),
        .g = (uint8_t)((vc.g * keep + fogC.g * ir0) >> 12),
        .b = (uint8_t)((vc.b * keep + fogC.b * ir0) >> 12),
    };
}


static inline psyqo::Color fogBaseColor(int32_t ir, psyqo::Color fogC) {
    if (ir <= 0) return {.r = 0, .g = 0, .b = 0};
    if (ir >= 4096) return fogC;
    return {
        .r = (uint8_t)((fogC.r * ir) >> 12),
        .g = (uint8_t)((fogC.g * ir) >> 12),
        .b = (uint8_t)((fogC.b * ir) >> 12),
    };
}

static inline psyqo::Color fogTexColor(psyqo::Color vc, int32_t ir) {
    if (ir <= 0) return vc;
    if (ir >= 4096) return {.r = 0, .g = 0, .b = 0};
    int32_t keep = 4096 - ir;
    return {
        .r = (uint8_t)((vc.r * keep) >> 12),
        .g = (uint8_t)((vc.g * keep) >> 12),
        .b = (uint8_t)((vc.b * keep) >> 12),
    };
}

// A white texel modulated by colour c gives c * 31 / 128 per 5-bit channel, so
// the mask pass draws with ~c / 2 (c * 33 / 64) to add what the untextured
// pass adds with c.
static inline psyqo::Color fogMaskTint(psyqo::Color c) {
    return {
        .r = (uint8_t)((c.r * 33 + 32) >> 6),
        .g = (uint8_t)((c.g * 33 + 32) >> 6),
        .b = (uint8_t)((c.b * 33 + 32) >> 6),
    };
}

// The fog is added over the darkened textured triangle. For a 4bpp or 8bpp
// cutout texture with a fog mask CLUT in VRAM, the fog pass is the same
// triangle drawn through that CLUT: entry 0 is transparent, so the texture's
// holes are skipped, and every other entry is white with STP set, so the
// additive blend applies to every texel the texture drew. Otherwise (16bpp,
// opaque textures, packs without mask CLUTs) it is an untextured triangle
// over the whole triangle.
void psxsplash::Renderer::insertFogPass(const Tri& tri, const psyqo::Vertex& a, const psyqo::Vertex& b,
                                        const psyqo::Vertex& c, psyqo::PrimPieces::UVCoords uvOffset,
                                        const int32_t fogIR[3], OT& ot, Balloc& balloc, int32_t zIndex) {
    psyqo::Color fogA = fogBaseColor(fogIR[0], m_fog.color);
    psyqo::Color fogB = fogBaseColor(fogIR[1], m_fog.color);
    psyqo::Color fogC = fogBaseColor(fogIR[2], m_fog.color);

    uint16_t mask = 0;
    if (tri.flags & CUTOUT_FLAG) {
        auto mode = tri.tpage.getColorMode();
        if (mode == psyqo::Prim::TPageAttr::Tex4Bits) mask = m_fog.maskClut4;
        else if (mode == psyqo::Prim::TPageAttr::Tex8Bits) mask = m_fog.maskClut8;
    }

    if (mask) {
        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
        p.primitive.pointA = a; p.primitive.pointB = b; p.primitive.pointC = c;
        p.primitive.uvA.u = tri.uvA.u + uvOffset.u; p.primitive.uvA.v = tri.uvA.v + uvOffset.v;
        p.primitive.uvB.u = tri.uvB.u + uvOffset.u; p.primitive.uvB.v = tri.uvB.v + uvOffset.v;
        p.primitive.uvC.u = tri.uvC.u + uvOffset.u; p.primitive.uvC.v = tri.uvC.v + uvOffset.v;
        p.primitive.tpage = tri.tpage;
        p.primitive.tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
        p.primitive.clutIndex = psyqo::PrimPieces::ClutIndex(mask & 0x3f, mask >> 6);
        p.primitive.setColorA(fogMaskTint(fogA)); p.primitive.setColorB(fogMaskTint(fogB)); p.primitive.setColorC(fogMaskTint(fogC));
        p.primitive.setSemiTrans();
        ot.insert(p, zIndex);
    } else {
        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
        p.primitive.pointA = a; p.primitive.pointB = b; p.primitive.pointC = c;
        p.primitive.setColorA(fogA); p.primitive.setColorB(fogB); p.primitive.setColorC(fogC);
        p.primitive.setSemiTrans();
        ot.insert(p, zIndex);
    }
}


void psxsplash::Renderer::beginFade(int32_t fogFarSZ) {
    m_edgeSZ = m_fog.edgeFade ? fade::edgeDepth(m_otSize, fogFarSZ) : 0;
    m_fadeStartSZ = fade::fadeStart(fogFarSZ, m_edgeSZ);
}

// Out of line: only triangles reaching past the fade start come here, and the
// per-triangle path stays small for the instruction cache.
__attribute__((noinline)) void psxsplash::Renderer::fadeTriangle(int32_t sz0, int32_t sz1, int32_t sz2, int32_t fogFarSZ,
                                                                 int32_t edge, int32_t fogIR[3]) {
    fogIR[0] = fade::vertexIR(sz0, fogFarSZ, edge);
    fogIR[1] = fade::vertexIR(sz1, fogFarSZ, edge);
    fogIR[2] = fade::vertexIR(sz2, fogFarSZ, edge);
}

namespace {

// Whether an edge between screen points (ax, ay) and (bx, by) at depths az
// and bz (> 0) warps by more than q quarter pixels: the texel of
// the 3D midpoint lands L*|bz - az| / (2*(az + bz)) pixels from the screen
// midpoint. L is approximated by max + min/2 of |dx| and |dy|, within 12% of
// the length (never under it). Symmetric in a and b. Unsigned products: the
// ends lie on the safe box, so L < 1300, and |dz| < 2^16: 2*L*|dz| < 2^28.
// Only edges with an end nearer than zs.
__attribute__((always_inline)) inline unsigned warpEdge(int32_t ax, int32_t ay, int32_t az, int32_t bx, int32_t by,
                                                       int32_t bz, int32_t q, int32_t zs) {
    uint32_t dx = (uint32_t)(ax > bx ? ax - bx : bx - ax);
    uint32_t dy = (uint32_t)(ay > by ? ay - by : by - ay);
    uint32_t dz = (uint32_t)(az > bz ? az - bz : bz - az);
    uint32_t l = dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
    return (az < zs || bz < zs) && 2 * l * dz > (uint32_t)(q * (az + bz)) ? 1u : 0u;
}

inline int32_t clampTo(int32_t v, int32_t m) { return v < -m ? -m : v > m ? m : v; }

}  // namespace

void psxsplash::Renderer::processTriangle(
    Tri& tri, int32_t fogFarSZ,
    OT& ot,
    Balloc& balloc,
    psyqo::PrimPieces::UVCoords uvOffset, int depth) {

    if (!(tri.flags & ANIM_FLAG)) {
        uvOffset = { 0,0 };
    }

    writeSafe<PseudoRegister::V0>(tri.v0);
    writeSafe<PseudoRegister::V1>(tri.v1);
    writeSafe<PseudoRegister::V2>(tri.v2);

    Kernels::rtpt();

    uint32_t u0, u1, u2;
    read<Register::SZ1>(&u0);
    read<Register::SZ2>(&u1);
    read<Register::SZ3>(&u2);
    int32_t sz0 = (int32_t)u0, sz1 = (int32_t)u1, sz2 = (int32_t)u2;

    // Entirely behind camera -> invisible.
    if (sz0 <= 0 && sz1 <= 0 && sz2 <= 0) return;
    // Entirely beyond fog wall -> invisible.
    if (fogFarSZ > 0 && sz0 > fogFarSZ && sz1 > fogFarSZ && sz2 > fogFarSZ) return;

    int32_t zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
    if (zIndex > m_peakZ) m_peakZ = zIndex;
    const int32_t maxZ = zIndex, minZ = eastl::min(eastl::min(sz0, sz1), sz2);
    // Past the table entirely -> dropped; straddling it -> the insert clamps it to the last bucket.
    if (minZ >= (int32_t)m_otSize) {
        RENDER_LOST(m_lost.ot++);
        return;
    }
    // Clamp above the reserved 2D bands, never to 0: geometry this close to the
    // camera must not land in the slots the sprites and UI draw from.
    if (zIndex < WORLD_DEPTH_MIN) zIndex = WORLD_DEPTH_MIN;

    psyqo::Vertex projected[3];
    read<Register::SXY0>(&projected[0].packed);
    read<Register::SXY1>(&projected[1].packed);
    read<Register::SXY2>(&projected[2].packed);

    // The GTE projects a vertex exactly only when SZ > H/2 (m_nearSZ and up):
    // below that its H/SZ division overflows and SXY comes out as if H/SZ were
    // 2. And the GPU drops a triangle spanning more than 1023x511 pixels. A
    // triangle reaching either goes through clipTriangle instead, which cuts
    // it exactly and projects the pieces itself.
    const int32_t nearSZ = m_nearSZ;
    bool nearPlane = (sz0 < nearSZ) | (sz1 < nearSZ) | (sz2 < nearSZ);
    bool offBox = outsideRasterizerRange(projected[0]) | outsideRasterizerRange(projected[1]) |
                  outsideRasterizerRange(projected[2]);

    if (!nearPlane) {
        // Off-screen reject: exact, the GTE saturates SXY but keeps its side.
        if (isCompletelyOutside(projected[0], projected[1], projected[2])) return;
        // Backface cull, unless SXY was saturated, which can flip the winding:
        // then the pieces are culled instead.
        if (!offBox || !gteSaturated(projected[0], projected[1], projected[2])) {
            Kernels::nclip();
            int32_t mac0 = 0;
            read<Register::MAC0>(reinterpret_cast<uint32_t*>(&mac0));
            if (mac0 <= 0) return;
        }
    }

    if (nearPlane | offBox) {
        RENDER_LOST(m_lost.clip++);
        // The clipper's trivial reject, decided from the GTE's own output when
        // it can be: wholly behind its near plane (SZ is z clamped at 0), or
        // wholly beyond one side of the screen by 3 pixels as the GTE projects
        // it, for corners with 0 < SZ < 0xffff. Where the GTE's divide overflowed (0 < SZ <= H/2) its SXY sits
        // about 2*x from the centre while the true one sits x*H/z >= 2*x
        // away, on the same side; elsewhere it is within a pixel. Either way
        // the corner is beyond that side by more than a pixel, as the
        // clipper's own test would find.
        auto outcode = [](const psyqo::Vertex& p, int32_t sz) {
            unsigned c = sz < CLIP_NEAR_Z ? 1u : 0u;
            if (sz > 0 && sz < 0xffff)  // at 0xffff SZ may have saturated, below the true z
                c |= (p.x < -3 ? 2u : 0u) | (p.x > 323 ? 4u : 0u) | (p.y < -3 ? 8u : 0u) | (p.y > 243 ? 16u : 0u);
            return c;
        };
        if (outcode(projected[0], sz0) & outcode(projected[1], sz1) & outcode(projected[2], sz2)) return;
    }

    // Affine-warp split. The edge test takes each corner at depth
    // max(SZ, CLIP_NEAR_Z) and on screen where the GTE put it, or, nearer than
    // H/2 where the GTE's SXY is wrong, projected in software from its view
    // position; either clamped onto the rasterizer's safe box. That is a
    // function of the corner alone, so the two triangles sharing an edge
    // decide it alike; the clamp measures roughly the part of the edge that
    // can be drawn. Only edges with an end nearer than m_splitZ qualify, so a
    // triangle wholly beyond it pays one compare. With the box 1023 pixels
    // wide, no edge can be split unless 3*1023*(maxZ - minZ) > 2*Q*minZ (the
    // test's L is at most 1.5 times the box side, |z1 - z0| at most
    // maxZ - minZ and z0 + z1 at least 2*minZ): level triangles pay that one
    // more. Inside the box, the larger side of the triangle's bounding box
    // replaces 1023.
    const int32_t zSplit = m_splitZ;
    const int32_t q = m_splitQ;
    if (depth < SPLIT_DEPTH && minZ < zSplit) {
        const int32_t zLo = nearPlane ? eastl::max(minZ, CLIP_NEAR_Z) : minZ;
        const int32_t zHi = nearPlane ? eastl::max(maxZ, CLIP_NEAR_Z) : maxZ;
        if (3 * (SAFE_MAX_X - SAFE_MIN_X) * (zHi - zLo) > 2 * q * zLo) {
            const int32_t szs[3] = {sz0, sz1, sz2};
            int32_t px[3], py[3], pz[3];
            bool test = true;
            if (!(nearPlane | offBox)) {
                for (int i = 0; i < 3; i++) {
                    px[i] = projected[i].x;
                    py[i] = projected[i].y;
                    pz[i] = szs[i];
                }
                const int32_t bw = eastl::max(eastl::max(px[0], px[1]), px[2]) - eastl::min(eastl::min(px[0], px[1]), px[2]);
                const int32_t bh = eastl::max(eastl::max(py[0], py[1]), py[2]) - eastl::min(eastl::min(py[0], py[1]), py[2]);
                test = 3 * eastl::max(bw, bh) * (zHi - zLo) > 2 * q * zLo;
            } else {
                int32_t vx[3], vy[3];
                if (nearPlane) {
                    // RTPT left the third corner's view position in MAC1-2.
                    vx[2] = (int32_t)readRaw<Register::MAC1, Safe>();
                    vy[2] = (int32_t)readRaw<Register::MAC2, Safe>();
                    if (sz0 < nearSZ) {
                        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
                        vx[0] = (int32_t)readRaw<Register::MAC1, Safe>();
                        vy[0] = (int32_t)readRaw<Register::MAC2, Safe>();
                    }
                    if (sz1 < nearSZ) {
                        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V1, Kernels::TV::TR>();
                        vx[1] = (int32_t)readRaw<Register::MAC1, Safe>();
                        vy[1] = (int32_t)readRaw<Register::MAC2, Safe>();
                    }
                    // clipTriangle reads V2's view position back from there.
                    if ((sz0 < nearSZ) | (sz1 < nearSZ))
                        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V2, Kernels::TV::TR>();
                }
                const int32_t h = m_projH;
                const int32_t ofx = (int32_t)readRaw<Register::OFX, Safe>() >> 16;  // 16.16
                const int32_t ofy = (int32_t)readRaw<Register::OFY, Safe>() >> 16;
                for (int i = 0; i < 3; i++) {
                    int32_t x, y;
                    if (szs[i] >= nearSZ) {
                        x = projected[i].x;
                        y = projected[i].y;
                        pz[i] = szs[i];
                    } else {
                        // |x| clamped under 2^21 keeps x*H (H < 1024) inside
                        // 32 bits; at z < H/2 that is still far off the box.
                        const int32_t z = eastl::max(szs[i], CLIP_NEAR_Z);
                        x = ofx + clampTo(clampTo(vx[i], (1 << 21) - 1) * h / z, 4096);
                        y = ofy + clampTo(clampTo(vy[i], (1 << 21) - 1) * h / z, 4096);
                        pz[i] = z;
                    }
                    px[i] = x < SAFE_MIN_X ? SAFE_MIN_X : x > SAFE_MAX_X ? SAFE_MAX_X : x;
                    py[i] = y < SAFE_MIN_Y ? SAFE_MIN_Y : y > SAFE_MAX_Y ? SAFE_MAX_Y : y;
                }
            }
            if (test) {
                unsigned edges = warpEdge(px[0], py[0], pz[0], px[1], py[1], pz[1], q, zSplit) |
                                 warpEdge(px[1], py[1], pz[1], px[2], py[2], pz[2], q, zSplit) << 1 |
                                 warpEdge(px[2], py[2], pz[2], px[0], py[0], pz[0], q, zSplit) << 2;
                if (edges && splitTriangle(tri, edges, fogFarSZ, ot, balloc, uvOffset, depth)) return;
            }
        }
    }

    if (nearPlane | offBox) {
        clipTriangle(tri, projected, fogFarSZ, ot, balloc, uvOffset);
        return;
    }

    const int32_t sz[3] = {sz0, sz1, sz2};
    emitTriangle(tri, projected[0], projected[1], projected[2], sz, zIndex, fogFarSZ, ot, balloc, uvOffset);
}

void psxsplash::Renderer::beginSplitFrame() {
    const int32_t kids = m_splitKids;
    m_splitKids = 0;
    if (kids > SPLIT_BUDGET) {
        m_splitQ = eastl::min(m_splitQ + (m_splitQ >> 2), SPLIT_ERR_Q_MAX);
    } else if (kids < (SPLIT_BUDGET >> 1) && m_splitQ > SPLIT_ERR_Q) {
        m_splitQ = eastl::max(m_splitQ - (m_splitQ >> 3), SPLIT_ERR_Q);
    }
    RENDER_LOST(if ((uint32_t)m_splitQ < m_lost.eqLo) m_lost.eqLo = (uint32_t)m_splitQ;
                if ((uint32_t)m_splitQ > m_lost.eqHi) m_lost.eqHi = (uint32_t)m_splitQ);
}

// The children of a split, by the edges split (bit i: corner i to i + 1),
// as corners: 0-2 the triangle's own, 3 + i the midpoint of edge i; winding
// kept. Two edges split leave a corner triangle and a quad, cut from the
// unsplit edge's first corner.
namespace {
struct SplitCorner {
    psyqo::GTE::PackedVec3 p;
    uint8_t u, v;
    psyqo::Color c;
};
constexpr uint8_t SPLIT_KIDS[8][4][3] = {
    {},
    {{0, 3, 2}, {3, 1, 2}},
    {{1, 4, 0}, {4, 2, 0}},
    {{3, 1, 4}, {2, 0, 3}, {2, 3, 4}},
    {{2, 5, 1}, {5, 0, 1}},
    {{5, 0, 3}, {1, 2, 5}, {1, 5, 3}},
    {{4, 2, 5}, {0, 1, 4}, {0, 4, 5}},
    {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}},
};
constexpr uint8_t SPLIT_COUNT[8] = {1, 2, 2, 3, 2, 3, 3, 4};
}  // namespace

bool psxsplash::Renderer::splitTriangle(const Tri& tri, unsigned edges, int32_t fogFarSZ, OT& ot, Balloc& balloc,
                                        psyqo::PrimPieces::UVCoords uvOffset, int depth) {
    const int n = SPLIT_COUNT[edges];
    // Only to keep the frame's primitives inside the bump allocator, which
    // asserts when full: a child that is clipped can draw up to six pieces,
    // each with a fog pass. Not reached in the measured scenes; counted.
    if (balloc.remaining() < (m_bumpSize >> 3)) {
        RENDER_LOST(m_lost.refused++);
        return false;
    }
    m_splitKids += n;
    RENDER_LOST(m_lost.split++);
    SplitCorner c[6];
    c[0] = {tri.v0, tri.uvA.u, tri.uvA.v, tri.colorA};
    c[1] = {tri.v1, tri.uvB.u, tri.uvB.v, tri.colorB};
    c[2] = {tri.v2, tri.uvC.u, tri.uvC.v, tri.colorC};
    Tri child = tri;
#if PSXSPLASH_FEATURE_LIGHTS
    // Lit once, at the corners: the children carry the lit colours, halved
    // at the midpoints as the GPU's Gouraud shading would have, instead of
    // each lighting its own corners again.
    if (m_objLights.count > 0 && !(tri.flags & SPLIT_LIT_FLAG)) {
        if (m_objLightsSmooth) applyPointLights(m_objLights, tri, c[0].c, c[1].c, c[2].c);
        else applyPointLightsFlat(m_objLights, tri, c[0].c, c[1].c, c[2].c);
        child.flags |= SPLIT_LIT_FLAG;
    }
#endif
    for (int i = 0; i < 3; i++) {
        if (!(edges & (1u << i))) continue;
        // Halved as in the old near-plane subdivision: symmetric in the two
        // ends, so the neighbour across the edge makes the same midpoint.
        const SplitCorner& a = c[i];
        const SplitCorner& b = c[i == 2 ? 0 : i + 1];
        SplitCorner& m = c[3 + i];
        m.p.x.value = (int16_t)(((int32_t)a.p.x.value + b.p.x.value) >> 1);
        m.p.y.value = (int16_t)(((int32_t)a.p.y.value + b.p.y.value) >> 1);
        m.p.z.value = (int16_t)(((int32_t)a.p.z.value + b.p.z.value) >> 1);
        m.u = (uint8_t)((a.u + b.u) >> 1);
        m.v = (uint8_t)((a.v + b.v) >> 1);
        m.c = a.c;
        m.c.r = (uint8_t)((a.c.r + b.c.r) >> 1);
        m.c.g = (uint8_t)((a.c.g + b.c.g) >> 1);
        m.c.b = (uint8_t)((a.c.b + b.c.b) >> 1);
    }
    for (int k = 0; k < n; k++) {
        const SplitCorner& a = c[SPLIT_KIDS[edges][k][0]];
        const SplitCorner& b = c[SPLIT_KIDS[edges][k][1]];
        const SplitCorner& d = c[SPLIT_KIDS[edges][k][2]];
        child.v0 = a.p; child.uvA.u = a.u; child.uvA.v = a.v; child.colorA = a.c;
        child.v1 = b.p; child.uvB.u = b.u; child.uvB.v = b.v; child.colorB = b.c;
        child.v2 = d.p; child.uvC.u = d.u; child.uvC.v = d.v; child.colorC = d.c;
        processTriangle(child, fogFarSZ, ot, balloc, uvOffset, depth + 1);
    }
    return true;
}

// The leaf both processTriangle and clipTriangle end in: per-vertex fog from
// sz, lights, and the primitives, at ordering-table depth zIndex.
void psxsplash::Renderer::emitTriangle(const Tri& tri, const psyqo::Vertex& p0, const psyqo::Vertex& p1,
                                       const psyqo::Vertex& p2, const int32_t sz[3], int32_t zIndex,
                                       int32_t fogFarSZ, OT& ot, Balloc& balloc,
                                       psyqo::PrimPieces::UVCoords uvOffset) {
    // Per-vertex fog and edge fade. zIndex is at least the largest SZ: a
    // triangle wholly before the fade start pays one compare.
    int32_t fogIR[3] = {0, 0, 0};
    if (zIndex > m_fadeStartSZ) fadeTriangle(sz[0], sz[1], sz[2], fogFarSZ, m_edgeSZ, fogIR);

    psyqo::Color cA = tri.colorA, cB = tri.colorB, cC = tri.colorC;
#if PSXSPLASH_FEATURE_LIGHTS
    if (m_objLights.count > 0 && !(tri.flags & SPLIT_LIT_FLAG)) {
        if (m_objLightsSmooth) applyPointLights(m_objLights, tri, cA, cB, cC);
        else applyPointLightsFlat(m_objLights, tri, cA, cB, cC);
    }
#endif
    bool hasFog = fogIR[0] > 0 || fogIR[1] > 0 || fogIR[2] > 0;

    if (tri.isUntextured()) {
        if (hasFog) {
            cA = fogBlend(cA, fogIR[0], m_fog.color);
            cB = fogBlend(cB, fogIR[1], m_fog.color);
            cC = fogBlend(cC, fogIR[2], m_fog.color);
        }
        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
        p.primitive.pointA = p0; p.primitive.pointB = p1; p.primitive.pointC = p2;
        p.primitive.setColorA(cA); p.primitive.setColorB(cB); p.primitive.setColorC(cC);
        p.primitive.setOpaque();
        ot.insert(p, zIndex);
    } else if (hasFog) {
        psyqo::Color texA = fogTexColor(cA, fogIR[0]);
        psyqo::Color texB = fogTexColor(cB, fogIR[1]);
        psyqo::Color texC = fogTexColor(cC, fogIR[2]);

        insertFogPass(tri, p0, p1, p2, uvOffset, fogIR, ot, balloc, zIndex);

        auto& texP = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
        texP.primitive.pointA = p0; texP.primitive.pointB = p1; texP.primitive.pointC = p2;
        texP.primitive.uvA.u = tri.uvA.u + uvOffset.u; texP.primitive.uvA.v = tri.uvA.v + uvOffset.v;
        texP.primitive.uvB.u = tri.uvB.u + uvOffset.u; texP.primitive.uvB.v = tri.uvB.v + uvOffset.v;
        texP.primitive.uvC.u = tri.uvC.u + uvOffset.u; texP.primitive.uvC.v = tri.uvC.v + uvOffset.v;
        texP.primitive.tpage = tri.tpage;
        texP.primitive.tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
        psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
        texP.primitive.clutIndex = clut;
        texP.primitive.setColorA(texA); texP.primitive.setColorB(texB); texP.primitive.setColorC(texC);
        texP.primitive.setOpaque();
        ot.insert(texP, zIndex);
    } else {

        auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();
        p.primitive.pointA = p0; p.primitive.pointB = p1; p.primitive.pointC = p2;
        p.primitive.uvA.u = tri.uvA.u + uvOffset.u; p.primitive.uvA.v = tri.uvA.v + uvOffset.v;
        p.primitive.uvB.u = tri.uvB.u + uvOffset.u; p.primitive.uvB.v = tri.uvB.v + uvOffset.v;
        p.primitive.uvC.u = tri.uvC.u + uvOffset.u; p.primitive.uvC.v = tri.uvC.v + uvOffset.v;
        p.primitive.tpage = tri.tpage;
        p.primitive.tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
        psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
        p.primitive.clutIndex = clut;
        p.primitive.setColorA(cA); p.primitive.setColorB(cB); p.primitive.setColorC(cC);
        p.primitive.setOpaque();
        ot.insert(p, zIndex);
    }
}


// ============================================================================
// Exact clipping
// ============================================================================

namespace {

// A polygon vertex while clipping: its view-space position (MAC1-3 of
// RT*V + TR, in SZ units, unsaturated), its model-space position (for the
// lights) and the attributes interpolated along with it. gte: the vertex is
// one of the triangle's own and the GTE projected it exactly, into sxy.
struct ClipVert {
    int32_t x, y, z;
    int16_t mx, my, mz;
    uint8_t u, v, r, g, b;
    bool gte;
    psyqo::Vertex sxy;
};

// View-space position of the model-space vertex in V0, V1 or V2: the same
// MAC1-3 RTPS computes before it projects.
template <Kernels::MV vreg>
inline void viewPosition(ClipVert& c) {
    Kernels::mvmva<Kernels::MX::RT, vreg, Kernels::TV::TR>();
    c.x = (int32_t)readRaw<Register::MAC1, Safe>();
    c.y = (int32_t)readRaw<Register::MAC2, Safe>();
    c.z = (int32_t)readRaw<Register::MAC3, Safe>();
}

inline ClipVert clipCorner(const psyqo::GTE::PackedVec3& p, psyqo::PrimPieces::UVCoords uv, psyqo::Color c) {
    ClipVert v;
    v.mx = p.x.value; v.my = p.y.value; v.mz = p.z.value;
    v.u = uv.u; v.v = uv.v;
    v.r = c.r; v.g = c.g; v.b = c.b;
    return v;
}

// a + (b - a) * t, t in 12-bit fixed point (0..4096).
inline int32_t clipLerp(int32_t a, int32_t b, int32_t t) { return a + (((b - a) * t + 2048) >> 12); }
// The same for view coordinates, whose difference times t can pass 32 bits:
// a 32x32->64 multiply, shifted back down.
inline int32_t clipLerpWide(int32_t a, int32_t b, int32_t t) {
    return a + (int32_t)(((int64_t)(b - a) * t + 2048) >> 12);
}

// floor(n / d) for d > 0, rounding as the GTE's projection does.
inline int32_t floorDiv(int32_t n, int32_t d) {
    int32_t q = n / d;
    return (q * d > n) ? q - 1 : q;
}

inline void setCorner(psyqo::GTE::PackedVec3& p, uint8_t& u, uint8_t& v, psyqo::Color& c, const ClipVert& from) {
    p.x.value = from.mx; p.y.value = from.my; p.z.value = from.mz;
    u = from.u; v = from.v;
    c.r = from.r; c.g = from.g; c.b = from.b;
}

}  // namespace

// Sutherland-Hodgman against five planes, linear in view space (and so affine
// in model space): the near plane z >= CLIP_NEAR_Z, and the four sides of the
// rasterizer's safe box, inset by CLIP_BOX_MARGIN, written without the
// division:
//   sx >= X0  <=>  H*x + (OFX - X0)*z >= 0   (for z > 0, which the near plane
// already ensured), and likewise for the other three. A new vertex is
// interpolated from the edge's inside end, so the two triangles sharing an
// edge cut it at the same point. The corners are then projected in software,
// except the triangle's own corners the GTE projected exactly, so a clipped
// triangle meets its unclipped neighbours on the same pixels; the pieces are
// drawn as a fan through emitTriangle.
void psxsplash::Renderer::clipTriangle(const Tri& tri, const psyqo::Vertex projected[3], int32_t fogFarSZ,
                                       OT& ot, Balloc& balloc, psyqo::PrimPieces::UVCoords uvOffset) {
    // RTPT left the third corner's view position in MAC1-3: RT*V2 + TR, the
    // same as viewPosition<V2> would compute (nclip only writes MAC0).
    const int32_t v2x = (int32_t)readRaw<Register::MAC1, Safe>();
    const int32_t v2y = (int32_t)readRaw<Register::MAC2, Safe>();
    const int32_t v2z = (int32_t)readRaw<Register::MAC3, Safe>();
    const int32_t h = (int32_t)(readRaw<Register::H, Safe>() & 0xffff);  // reads back sign extended
    const int32_t ofx = (int32_t)readRaw<Register::OFX, Safe>() >> 16;  // 16.16
    const int32_t ofy = (int32_t)readRaw<Register::OFY, Safe>() >> 16;
    // Plane distances, >= 0 inside: z - CLIP_NEAR_Z for the near plane, then
    // k*(x or y) + c*z for the box sides, k = +-H.
    const int32_t sideK[4] = {h, -h, h, -h};
    const int32_t sideC[4] = {ofx - (SAFE_MIN_X + CLIP_BOX_MARGIN), (SAFE_MAX_X - CLIP_BOX_MARGIN) - ofx,
                              ofy - (SAFE_MIN_Y + CLIP_BOX_MARGIN), (SAFE_MAX_Y - CLIP_BOX_MARGIN) - ofy};

    // A triangle cut by five planes has at most 3 + 5 corners.
    ClipVert bufA[8], bufB[8];
    ClipVert* poly = bufA;
    ClipVert* next = bufB;
    poly[0] = clipCorner(tri.v0, tri.uvA, tri.colorA);
    poly[1] = clipCorner(tri.v1, tri.uvB, tri.colorB);
    poly[2] = clipCorner(tri.v2, {tri.uvC.u, tri.uvC.v}, tri.colorC);
    // processTriangle left the triangle in V0-V2.
    viewPosition<Kernels::MV::V0>(poly[0]);
    viewPosition<Kernels::MV::V1>(poly[1]);
    poly[2].x = v2x;
    poly[2].y = v2y;
    poly[2].z = v2z;
    for (int i = 0; i < 3; i++) {
        ClipVert& c = poly[i];
        // Exact when SZ is past the divide overflow and did not saturate,
        // IR1/IR2 did not saturate, and the GPU can take it.
        c.gte = c.z >= m_nearSZ && c.z <= 0xffff && c.x >= -0x8000 && c.x < 0x8000 && c.y >= -0x8000 && c.y < 0x8000 &&
                !outsideRasterizerRange(projected[i]);
        c.sxy = projected[i];
    }
    int n = 3;
    psyqo::Vertex sxy[8];
    int32_t sz[8];

    // Trivial reject: all three corners behind the near plane, or all beyond
    // one side of the visible screen widened by a pixel. The sides are planes
    // through the eye (H*x + (OFX + 1)*z < 0 is sx < -1 for z > 0), so the test
    // holds whatever the corners' z: the whole triangle, and every piece S-H
    // would cut from it, lies beyond that side, where the fan's screen test
    // would drop it.
    {
        // In 32 bits when |x|, |y|, |z| < 2^21 and H < 512 (each product under
        // 2^30, each sum under 2^31, for an OFX/OFY inside the screen), else 64.
        auto small = [](int32_t v) { return (uint32_t)(v + (1 << 21)) < (2u << 21); };
        bool narrow = h < 512 && (uint32_t)ofx <= 320 && (uint32_t)ofy <= 240;
        for (int i = 0; i < 3; i++) narrow &= small(poly[i].x) & small(poly[i].y) & small(poly[i].z);
        unsigned all = 0x1f;
        for (int i = 0; i < 3; i++) {
            const ClipVert& c = poly[i];
            unsigned oc = c.z < CLIP_NEAR_Z ? 1u : 0u;
            if (narrow) {
                const int32_t hx = h * c.x, hy = h * c.y;
                oc |= (hx + (ofx + 1) * c.z < 0 ? 2u : 0u) | (hx - (321 - ofx) * c.z > 0 ? 4u : 0u) |
                      (hy + (ofy + 1) * c.z < 0 ? 8u : 0u) | (hy - (241 - ofy) * c.z > 0 ? 16u : 0u);
            } else {
                const int64_t hx = (int64_t)h * c.x, hy = (int64_t)h * c.y;
                oc |= (hx + (int64_t)(ofx + 1) * c.z < 0 ? 2u : 0u) | (hx - (int64_t)(321 - ofx) * c.z > 0 ? 4u : 0u) |
                      (hy + (int64_t)(ofy + 1) * c.z < 0 ? 8u : 0u) | (hy - (int64_t)(241 - ofy) * c.z > 0 ? 16u : 0u);
            }
            all &= oc;
        }
        if (all) return;
    }

    // Fast path: a triangle no plane cuts, which is most of them (near the
    // camera, not crossing the near plane): project the corners and draw it
    // whole. Inside a plane is tested on the projection, exactly as the plane
    // distance would: floor(v) >= X0 iff v >= X0, and floor(v) < X1 implies
    // v < X1 (a corner landing on X1 itself takes the slow path). The GTE's
    // own SXY is within a pixel or two of floor(v), so it must be 3 pixels in.
    constexpr int32_t X0 = SAFE_MIN_X + CLIP_BOX_MARGIN, X1 = SAFE_MAX_X - CLIP_BOX_MARGIN;
    constexpr int32_t Y0 = SAFE_MIN_Y + CLIP_BOX_MARGIN, Y1 = SAFE_MAX_Y - CLIP_BOX_MARGIN;
    bool fast = h < 512;
    for (int i = 0; i < 3 && fast; i++) {
        const ClipVert& c = poly[i];
        sz[i] = c.z > 0xffff ? 0xffff : c.z;
        if (c.z < CLIP_NEAR_Z) {
            fast = false;
        } else if (c.gte) {
            sxy[i] = c.sxy;
            fast = c.sxy.x >= X0 + 3 && c.sxy.x <= X1 - 3 && c.sxy.y >= Y0 + 3 && c.sxy.y <= Y1 - 3;
        } else {
            // |x|, |y| < 2^22 and H < 512 keep x*H and y*H inside 32 bits;
            // z < 2^21 is the projection loop's bound, below which it does not
            // halve, so this is its result to the bit.
            fast = c.z < (1 << 21) && (uint32_t)(c.x + (1 << 22)) < (2u << 22) &&
                   (uint32_t)(c.y + (1 << 22)) < (2u << 22);
            if (fast) {
                int32_t sx = ofx + floorDiv(c.x * h, c.z), sy = ofy + floorDiv(c.y * h, c.z);
                sxy[i].x = (int16_t)sx;
                sxy[i].y = (int16_t)sy;
                fast = sx >= X0 && sx < X1 && sy >= Y0 && sy < Y1;
            }
        }
    }

    if (!fast) {
        for (int p = 0; p < 5; p++) {
            int64_t d[8];
            int inside = 0;
            if (p == 0) {
                for (int i = 0; i < n; i++) {
                    d[i] = poly[i].z - CLIP_NEAR_Z;
                    inside += d[i] >= 0;
                }
            } else {
                const int32_t k = sideK[p - 1], c = sideC[p - 1];
                const bool useY = p >= 3;
                for (int i = 0; i < n; i++) {
                    d[i] = (int64_t)k * (useY ? poly[i].y : poly[i].x) + (int64_t)c * poly[i].z;
                    inside += d[i] >= 0;
                }
            }
            if (inside == n) continue;
            if (inside == 0) return;
            int m = 0;
            for (int i = 0; i < n; i++) {
                int j = i + 1 == n ? 0 : i + 1;
                bool inI = d[i] >= 0, inJ = d[j] >= 0;
                if (inI) next[m++] = poly[i];
                if (inI == inJ) continue;
                const ClipVert& a = inI ? poly[i] : poly[j];
                const ClipVert& b = inI ? poly[j] : poly[i];
                int64_t da = inI ? d[i] : d[j];     // >= 0
                int64_t db = -(inI ? d[j] : d[i]);  // > 0
                // t = da / (da + db) in 12 bits, with 32-bit division only.
                while ((uint64_t)(da + db) >> 18) {
                    da >>= 4;
                    db >>= 4;
                }
                int32_t t = ((int32_t)da << 12) / (int32_t)(da + db);
                ClipVert& c = next[m++];
                c.x = clipLerpWide(a.x, b.x, t);
                c.y = clipLerpWide(a.y, b.y, t);
                c.z = clipLerpWide(a.z, b.z, t);
                c.mx = (int16_t)clipLerp(a.mx, b.mx, t);
                c.my = (int16_t)clipLerp(a.my, b.my, t);
                c.mz = (int16_t)clipLerp(a.mz, b.mz, t);
                c.u = (uint8_t)clipLerp(a.u, b.u, t);
                c.v = (uint8_t)clipLerp(a.v, b.v, t);
                c.r = (uint8_t)clipLerp(a.r, b.r, t);
                c.g = (uint8_t)clipLerp(a.g, b.g, t);
                c.b = (uint8_t)clipLerp(a.b, b.b, t);
                c.gte = false;
            }
            ClipVert* t = poly; poly = next; next = t;
            n = m;
        }

        // Project: sx = OFX + x*H/z, sy = OFY + y*H/z. Every corner is inside the
        // inset box, so |x*H| <= 512*z and |y*H| <= 256*z (give or take the
        // rounding of a few units); halving x, y and z together until z < 2^21
        // keeps x*H and y*H under 2^30, inside 32 bits, and the ratio unchanged.
        for (int i = 0; i < n; i++) {
            const ClipVert& c = poly[i];
            sz[i] = c.z > 0xffff ? 0xffff : c.z;
            if (c.gte) {
                sxy[i] = c.sxy;
                continue;
            }
            int32_t x = c.x, y = c.y, z = c.z;
            while (z >= (1 << 21)) {
                x >>= 1;
                y >>= 1;
                z >>= 1;
            }
            sxy[i].x = (int16_t)(ofx + floorDiv(x * h, z));
            sxy[i].y = (int16_t)(ofy + floorDiv(y * h, z));
        }
    }

    for (int i = 1; i + 1 < n; i++) {
        const psyqo::Vertex& a = sxy[0];
        const psyqo::Vertex& b = sxy[i];
        const psyqo::Vertex& c = sxy[i + 1];
        // Off-screen reject against the visible screen, then the backface
        // test: the signed area nclip would give, from the software SXY.
        if ((a.x < 0 && b.x < 0 && c.x < 0) || (a.x >= 320 && b.x >= 320 && c.x >= 320) ||
            (a.y < 0 && b.y < 0 && c.y < 0) || (a.y >= 240 && b.y >= 240 && c.y >= 240))
            continue;
        int32_t area = (int32_t)a.x * b.y + (int32_t)b.x * c.y + (int32_t)c.x * a.y - (int32_t)a.x * c.y -
                       (int32_t)b.x * a.y - (int32_t)c.x * b.y;
        if (area <= 0) continue;
        Tri piece = tri;
        setCorner(piece.v0, piece.uvA.u, piece.uvA.v, piece.colorA, poly[0]);
        setCorner(piece.v1, piece.uvB.u, piece.uvB.v, piece.colorB, poly[i]);
        setCorner(piece.v2, piece.uvC.u, piece.uvC.v, piece.colorC, poly[i + 1]);
        // Ordering-table depth: the piece's deepest view z, floored as processTriangle does.
        const int32_t psz[3] = {sz[0], sz[i], sz[i + 1]};
        int32_t zIndex = eastl::max(eastl::max(psz[0], psz[1]), psz[2]);
        if (zIndex < WORLD_DEPTH_MIN) zIndex = WORLD_DEPTH_MIN;
        emitTriangle(piece, a, b, c, psz, zIndex, fogFarSZ, ot, balloc, uvOffset);
    }
}

// ============================================================================
// Render paths
// ============================================================================

void psxsplash::Renderer::Render(eastl::vector<GameObject*>& objects) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");
    // Re-sync GTE H register each frame (supports dynamic FOV / cutscene H tracks)
    setProjectionH(m_currentCamera->GetProjectionH());
    beginSplitFrame();
    uint8_t parity = m_gpu.getParity();
    auto& ot = *m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = *m_ballocs[parity];
    notePeakBump(balloc);
    balloc.reset();
    auto& ditherCmd = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd.primitive.attr.setDithering(true);
    ditherCmd.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
    ot.insert(ditherCmd, m_otSize - 1);

    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    beginFade(fogFarSZ);
    for (auto& obj : objects) {
        if (!obj->isActive()) continue;
        if (obj->isSkinned()) continue;
        setupObjectTransform(obj, cameraPosition);
        RENDER_LOST(m_lost.tris += obj->polyCount);
        for (int i = 0; i < obj->polyCount; i++)
            processTriangle(obj->polygons[i], fogFarSZ, ot, balloc, obj->uvOffset);
    }
#if PSXSPLASH_FEATURE_SKIN
    renderSkinnedObjects(objects, cameraPosition, fogFarSZ, ot, balloc);
#endif
#if PSXSPLASH_FEATURE_SHADOWS
    renderShadows(objects, cameraPosition, fogFarSZ, ot, balloc);
#endif
    // Sprites go in before the UI: both head-insert, so inserting the UI
    // later is what keeps it on top within its band.
#if PSXSPLASH_FEATURE_SPRITES
    if (m_tileSystem) m_tileSystem->renderOT(ot, balloc);
    if (m_spriteSystem) m_spriteSystem->renderOT(ot, balloc);
#endif
#if PSXSPLASH_FEATURE_UI
    if (m_uiSystem) m_uiSystem->renderOT(m_gpu, ot, balloc);
#endif
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderOT(ot, balloc);
#endif
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().renderOT(ot, balloc);
#endif
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    // Last insert into the far bucket, so the CLUT uploads precede every primitive of the frame.
    if (m_paletteCycler) m_paletteCycler->emit(ot, balloc, int32_t(m_otSize) - 1);
    m_gpu.chain(clear); m_gpu.chain(ot);
#if PSXSPLASH_FEATURE_UI
    if (m_uiSystem) m_uiSystem->renderText(m_gpu);
#endif
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderText(m_gpu);
#endif
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().renderText(m_gpu);
#endif
    reportPeaks();
    m_frameCount++;
}

void psxsplash::Renderer::RenderWithBVH(eastl::vector<GameObject*>& objects, const BVHManager& bvh) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");
    if (!bvh.isLoaded()) { Render(objects); return; }
    // Re-sync GTE H register each frame (supports dynamic FOV / cutscene H tracks)
    setProjectionH(m_currentCamera->GetProjectionH());
    beginSplitFrame();
    uint8_t parity = m_gpu.getParity();
    auto& ot = *m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = *m_ballocs[parity];
    notePeakBump(balloc);
    balloc.reset();
    auto& ditherCmd2 = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd2.primitive.attr.setDithering(true);
    ditherCmd2.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
    ot.insert(ditherCmd2, m_otSize - 1);

    Frustum frustum; m_currentCamera->ExtractFrustum(frustum);
    int visibleCount = bvh.cullFrustum(frustum, m_visibleRefs, m_visibleCapacity);
    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    beginFade(fogFarSZ);
    int16_t lastObjectIndex = -1;
    bool lastObjCulled = false;
    RENDER_LOST(uint32_t submitted = 0);
    for (int i = 0; i < visibleCount; i++) {
        const TriangleRef& ref = m_visibleRefs[i];
        if (ref.objectIndex >= objects.size()) continue;
        GameObject* obj = objects[ref.objectIndex];
        if (!obj->isActive()) continue;
        if (obj->isDynamicMoved()) continue;  // Skip dynamic objects in BVH pass - rendered below
        if (obj->isSkinned()) continue;  // Skip skinned objects - rendered in skinned pass
        if (ref.triangleIndex >= obj->polyCount) continue;
        if (ref.objectIndex != lastObjectIndex) {
            lastObjectIndex = ref.objectIndex;

            BVHNode objBox;
            objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
            objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
            if (!frustum.testAABB(objBox)) {
                lastObjCulled = true;
                continue;
            }
            lastObjCulled = false;
            setupObjectTransform(obj, cameraPosition);
        }
        if (lastObjCulled) continue;
        // polyCount is 0 while a streamed object's geometry is not resident.
        if (ref.triangleIndex >= obj->polyCount) continue;
        RENDER_LOST(submitted++);
        processTriangle(obj->polygons[ref.triangleIndex], fogFarSZ, ot, balloc, obj->uvOffset);
    }
    RENDER_LOST(m_lost.tris += submitted);

    // Second pass: render dynamically-moved objects (their BVH references are stale).
    // Uses per-object AABB frustum test then renders all triangles - like the non-BVH path.
    for (size_t oi = 0; oi < objects.size(); oi++) {
        GameObject* obj = objects[oi];
        if (!obj->isActive() || !obj->isDynamicMoved()) continue;
        if (obj->isSkinned()) continue;
        BVHNode objBox;
        objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
        objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
        if (!frustum.testAABB(objBox)) continue;
        setupObjectTransform(obj, cameraPosition);
        RENDER_LOST(m_lost.tris += obj->polyCount);
        for (int t = 0; t < obj->polyCount; t++) {
            processTriangle(obj->polygons[t], fogFarSZ, ot, balloc, obj->uvOffset);
        }
    }

#if PSXSPLASH_FEATURE_SKIN
    renderSkinnedObjects(objects, cameraPosition, fogFarSZ, ot, balloc, &frustum);
#endif
#if PSXSPLASH_FEATURE_SHADOWS
    renderShadows(objects, cameraPosition, fogFarSZ, ot, balloc);
#endif

    // Sprites go in before the UI: both head-insert, so inserting the UI
    // later is what keeps it on top within its band.
#if PSXSPLASH_FEATURE_SPRITES
    if (m_tileSystem) m_tileSystem->renderOT(ot, balloc);
    if (m_spriteSystem) m_spriteSystem->renderOT(ot, balloc);
#endif
#if PSXSPLASH_FEATURE_UI
    if (m_uiSystem) m_uiSystem->renderOT(m_gpu, ot, balloc);
#endif
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderOT(ot, balloc);
#endif
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().renderOT(ot, balloc);
#endif
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    // Last insert into the far bucket, so the CLUT uploads precede every primitive of the frame.
    if (m_paletteCycler) m_paletteCycler->emit(ot, balloc, int32_t(m_otSize) - 1);
    m_gpu.chain(clear); m_gpu.chain(ot);
#if PSXSPLASH_FEATURE_UI
    if (m_uiSystem) m_uiSystem->renderText(m_gpu);
#endif
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderText(m_gpu);
#endif
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().renderText(m_gpu);
#endif
    reportPeaks();
    m_frameCount++;
}

// ============================================================================
// RenderWithRooms - Portal/room occlusion for interior scenes
// ============================================================================

struct ScreenRect { int16_t minX, minY, maxX, maxY; };

static inline bool intersectRect(const ScreenRect& a, const ScreenRect& b, ScreenRect& out) {
    out.minX = (a.minX > b.minX) ? a.minX : b.minX; out.minY = (a.minY > b.minY) ? a.minY : b.minY;
    out.maxX = (a.maxX < b.maxX) ? a.maxX : b.maxX; out.maxY = (a.maxY < b.maxY) ? a.maxY : b.maxY;
    return out.minX < out.maxX && out.minY < out.maxY;
}

// Safety margin added to portal screen rects (pixels).
// Prevents geometry from popping at portal edges due to fixed-point rounding.
static constexpr int16_t PORTAL_MARGIN = 16;

// Transform a world-space point to camera space using the view rotation matrix.
static inline void worldToCamera(int32_t wx, int32_t wy, int32_t wz,
    int32_t camX, int32_t camY, int32_t camZ,
    const psyqo::Matrix33& camRot,
    int32_t& outX, int32_t& outY, int32_t& outZ) {
    int32_t rx = wx - camX, ry = wy - camY, rz = wz - camZ;
    outX = (int32_t)(((int64_t)camRot.vs[0].x.value * rx + (int64_t)camRot.vs[0].y.value * ry +
                       (int64_t)camRot.vs[0].z.value * rz) >> 12);
    outY = (int32_t)(((int64_t)camRot.vs[1].x.value * rx + (int64_t)camRot.vs[1].y.value * ry +
                       (int64_t)camRot.vs[1].z.value * rz) >> 12);
    outZ = (int32_t)(((int64_t)camRot.vs[2].x.value * rx + (int64_t)camRot.vs[2].y.value * ry +
                       (int64_t)camRot.vs[2].z.value * rz) >> 12);
}

// Project a camera-space point to screen coordinates. Returns false if behind near plane.
static inline bool projectToScreen(int32_t vx, int32_t vy, int32_t vz,
    int32_t projH, int16_t& sx, int16_t& sy) {
    if (vz <= 0) return false;
    int32_t vzs = vz >> 4; if (vzs <= 0) vzs = 1;
    int32_t rawX = (vx >> 4) * projH / vzs + 160;
    int32_t rawY = (vy >> 4) * projH / vzs + 120;
    if (rawX < -2048) rawX = -2048; else if (rawX > 2048) rawX = 2048;
    if (rawY < -2048) rawY = -2048; else if (rawY > 2048) rawY = 2048;
    sx = (int16_t)rawX;
    sy = (int16_t)rawY;
    return true;
}

// Project a portal quad to a screen-space AABB.
// Computes the 4 corners, transforms to camera space, clips against the near plane,
// projects visible points to screen, and returns the bounding rect.
static bool projectPortalRect(const psxsplash::PortalData& portal,
    int32_t camX, int32_t camY, int32_t camZ, const psyqo::Matrix33& camRot,
    int32_t projH, ScreenRect& outRect) {

    // Compute portal corner offsets in world space.
    int32_t rwx = ((int32_t)portal.rightX * portal.halfW) >> 12;
    int32_t rwy = ((int32_t)portal.rightY * portal.halfW) >> 12;
    int32_t rwz = ((int32_t)portal.rightZ * portal.halfW) >> 12;
    int32_t uhx = ((int32_t)portal.upX * portal.halfH) >> 12;
    int32_t uhy = ((int32_t)portal.upY * portal.halfH) >> 12;
    int32_t uhz = ((int32_t)portal.upZ * portal.halfH) >> 12;

    int32_t cx = portal.centerX, cy = portal.centerY, cz = portal.centerZ;

    // Transform 4 corners to camera space
    struct CamVert { int32_t x, y, z; };
    CamVert cv[4];
    int32_t wCorners[4][3] = {
        {cx + rwx + uhx, cy + rwy + uhy, cz + rwz + uhz},
        {cx - rwx + uhx, cy - rwy + uhy, cz - rwz + uhz},
        {cx - rwx - uhx, cy - rwy - uhy, cz - rwz - uhz},
        {cx + rwx - uhx, cy + rwy - uhy, cz + rwz - uhz},
    };

    int behindCount = 0;
    for (int i = 0; i < 4; i++) {
        worldToCamera(wCorners[i][0], wCorners[i][1], wCorners[i][2],
                      camX, camY, camZ, camRot, cv[i].x, cv[i].y, cv[i].z);
        if (cv[i].z <= 0) behindCount++;
    }

    // Any corner behind camera -> conservative fullscreen rect (unless portal center is far behind).
    if (behindCount > 0) {
        int32_t vx, vy, vz;
        worldToCamera(cx, cy, cz, camX, camY, camZ, camRot, vx, vy, vz);
        int32_t portalExtent = portal.halfW > portal.halfH ? portal.halfW : portal.halfH;
        int32_t distLimit = (behindCount == 4) ? portalExtent * 2 : portalExtent * 4;
        if (vz < 0 && -vz > distLimit) return false;
        outRect = {-512, -512, 832, 752};
        return true;
    }

    // Clip against near plane (z=1) and project visible points.
    // For each edge where one vertex is in front and one behind,
    // compute the intersection point and include it in the screen rect.
    constexpr int32_t NEAR_Z = 1;
    int16_t sxMin = 32767, sxMax = -32768;
    int16_t syMin = 32767, syMax = -32768;
    int projCount = 0;

    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;

        // Project vertex i if in front
        if (cv[i].z > 0) {
            int16_t sx, sy;
            if (projectToScreen(cv[i].x, cv[i].y, cv[i].z, projH, sx, sy)) {
                if (sx < sxMin) sxMin = sx;
                if (sx > sxMax) sxMax = sx;
                if (sy < syMin) syMin = sy;
                if (sy > syMax) syMax = sy;
                projCount++;
            }
        }

        // If edge crosses the near plane, clip and project the intersection.
        // All 32-bit arithmetic (no __divdi3 on MIPS R3000).
        bool iFront = cv[i].z > 0;
        bool jFront = cv[j].z > 0;
        if (iFront != jFront) {
            int32_t dz = cv[j].z - cv[i].z;
            if (dz == 0) continue;
            int32_t dzs = dz >> 4;
            if (dzs == 0) dzs = (dz > 0) ? 1 : -1;  // prevent div-by-zero after shift
            // Compute t in 4.12 fixed-point. Shift num/den by 4 to keep * 4096 in 32 bits.
            int32_t t12 = (((NEAR_Z - cv[i].z) >> 4) * 4096) / dzs;
            // Apply t: clip = cv[i] + (cv[j] - cv[i]) * t12 / 4096
            // Shift dx by 4 so (dx>>4)*t12 fits int32, then >>8 to undo (4+8=12 total)
            int32_t clipX = cv[i].x + ((((cv[j].x - cv[i].x) >> 4) * t12) >> 8);
            int32_t clipY = cv[i].y + ((((cv[j].y - cv[i].y) >> 4) * t12) >> 8);
            int16_t sx, sy;
            if (projectToScreen(clipX, clipY, NEAR_Z, projH, sx, sy)) {
                if (sx < sxMin) sxMin = sx;
                if (sx > sxMax) sxMax = sx;
                if (sy < syMin) syMin = sy;
                if (sy > syMax) syMax = sy;
                projCount++;
            }
        }
    }

    if (projCount == 0) return false;

    outRect = {
        (int16_t)(sxMin - PORTAL_MARGIN), (int16_t)(syMin - PORTAL_MARGIN),
        (int16_t)(sxMax + PORTAL_MARGIN), (int16_t)(syMax + PORTAL_MARGIN)
    };
    return true;
}

// Test if a room's AABB is potentially visible to the camera frustum.
// Quick rejection test: if the room is entirely behind the camera, skip it.
static bool isRoomPotentiallyVisible(const psxsplash::RoomData& room,
    int32_t camX, int32_t camY, int32_t camZ, const psyqo::Matrix33& camRot) {
    // Transform the room's AABB center to camera space and check Z.
    // Use the p-vertex approach: find the corner most in the camera forward direction.
    int32_t fwdX = camRot.vs[2].x.value;
    int32_t fwdY = camRot.vs[2].y.value;
    int32_t fwdZ = camRot.vs[2].z.value;

    // p-vertex: corner of AABB closest to camera forward direction
    int32_t px = (fwdX >= 0) ? room.aabbMaxX : room.aabbMinX;
    int32_t py = (fwdY >= 0) ? room.aabbMaxY : room.aabbMinY;
    int32_t pz = (fwdZ >= 0) ? room.aabbMaxZ : room.aabbMinZ;

    // If p-vertex is behind camera, the entire AABB is behind
    int32_t rx = px - camX, ry = py - camY, rz = pz - camZ;
    int64_t dotFwd = ((int64_t)fwdX * rx + (int64_t)fwdY * ry + (int64_t)fwdZ * rz) >> 12;
    if (dotFwd < -4096) return false;  // Entirely behind with 1-unit margin

    return true;
}

// Project an axis-aligned bounding box to a screen-space bounding rectangle.
// Used to test whether a cell's geometry could contribute to the visible portal area.
// Returns false if the AABB is entirely behind the camera.
static bool projectAABBToScreen(
    int32_t bMinX, int32_t bMinY, int32_t bMinZ,
    int32_t bMaxX, int32_t bMaxY, int32_t bMaxZ,
    int32_t camX, int32_t camY, int32_t camZ,
    const psyqo::Matrix33& camRot, int32_t projH,
    ScreenRect& outRect) {

    int16_t sxMin = 32767, sxMax = -32768;
    int16_t syMin = 32767, syMax = -32768;
    int behindCount = 0;

    for (int i = 0; i < 8; i++) {
        int32_t wx = (i & 1) ? bMaxX : bMinX;
        int32_t wy = (i & 2) ? bMaxY : bMinY;
        int32_t wz = (i & 4) ? bMaxZ : bMinZ;
        int32_t vx, vy, vz;
        worldToCamera(wx, wy, wz, camX, camY, camZ, camRot, vx, vy, vz);
        if (vz <= 0) { behindCount++; continue; }
        int16_t sx, sy;
        if (projectToScreen(vx, vy, vz, projH, sx, sy)) {
            if (sx < sxMin) sxMin = sx;
            if (sx > sxMax) sxMax = sx;
            if (sy < syMin) syMin = sy;
            if (sy > syMax) syMax = sy;
        }
    }

    if (behindCount == 8) return false;  // Entirely behind camera
    if (behindCount > 0 || sxMin > sxMax) {
        // Partially behind or no projected points: report conservative fullscreen
        outRect = {-512, -512, 832, 752};
        return true;
    }
    outRect = {sxMin, syMin, sxMax, syMax};
    return true;
}

// Minimum visible portal dimension (pixels) before a room is rendered.
// Portals smaller than this on screen are skipped to avoid rendering
// entire rooms through tiny distant slivers or nearly-closed doors.
static constexpr int16_t MIN_PORTAL_SCREEN_DIM = 8;

void psxsplash::Renderer::RenderWithRooms(eastl::vector<GameObject*>& objects,
    const RoomData* rooms, int roomCount, const PortalData* portals, int portalCount,
    const TriangleRef* roomTriRefs, const RoomCell* cells,
    const RoomPortalRef* roomPortalRefs, int cameraRoom) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");
    if (roomCount == 0 || rooms == nullptr) { Render(objects); return; }
    // Re-sync GTE H register each frame (supports dynamic FOV / cutscene H tracks)
    setProjectionH(m_currentCamera->GetProjectionH());
    beginSplitFrame();

    uint8_t parity = m_gpu.getParity();
    auto& ot = *m_ots[parity]; auto& clear = m_clear[parity]; auto& balloc = *m_ballocs[parity];
    notePeakBump(balloc);
    balloc.reset();
    auto& ditherCmd3 = balloc.allocateFragment<psyqo::Prim::TPage>();
    ditherCmd3.primitive.attr.setDithering(true);
    ditherCmd3.primitive.attr.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
    ot.insert(ditherCmd3, m_otSize - 1);

    psyqo::Vec3 cameraPosition = computeCameraViewPos();
    int32_t fogFarSZ = m_fog.fogFarSZ;
    beginFade(fogFarSZ);
    int32_t camX = m_currentCamera->GetPosition().x.raw();
    int32_t camY = m_currentCamera->GetPosition().y.raw();
    int32_t camZ = m_currentCamera->GetPosition().z.raw();
    int32_t projH = m_currentCamera->GetProjectionH();
    Frustum frustum; m_currentCamera->ExtractFrustum(frustum);
    int catchAllIdx = roomCount - 1;

    // If no camera room provided (or invalid), fall back to AABB containment.
    // Pick the smallest room whose AABB (with margin) contains the camera.
    if (cameraRoom < 0 || cameraRoom >= catchAllIdx) {
        constexpr int32_t ROOM_MARGIN = 2048;  // 0.5 units in fp12
        int64_t bestVolume = 0x7FFFFFFFFFFFFFFFLL;
        for (int r = 0; r < catchAllIdx; r++) {
            if (camX >= rooms[r].aabbMinX - ROOM_MARGIN && camX <= rooms[r].aabbMaxX + ROOM_MARGIN &&
                camY >= rooms[r].aabbMinY - ROOM_MARGIN && camY <= rooms[r].aabbMaxY + ROOM_MARGIN &&
                camZ >= rooms[r].aabbMinZ - ROOM_MARGIN && camZ <= rooms[r].aabbMaxZ + ROOM_MARGIN) {
                int64_t dx = (int64_t)(rooms[r].aabbMaxX - rooms[r].aabbMinX);
                int64_t dy = (int64_t)(rooms[r].aabbMaxY - rooms[r].aabbMinY);
                int64_t dz = (int64_t)(rooms[r].aabbMaxZ - rooms[r].aabbMinZ);
                int64_t vol = dx * dy + dy * dz + dx * dz;
                if (vol < bestVolume) { bestVolume = vol; cameraRoom = r; }
            }
        }
    }

    uint32_t visited = 0;
    if (catchAllIdx < 32) visited = (1u << catchAllIdx);
    const auto& camRot = m_currentCamera->GetRotation();

    struct Entry { int room; int depth; ScreenRect clip; };
    Entry stack[64]; int top = 0;

    // Helper: render a span of tri-refs with per-object frustum culling.
    // lastObj/lastObjCulled are managed across the span to avoid redundant
    // object transforms when consecutive refs share an object.
    auto renderTriRefs = [&](const TriangleRef* refs, int count,
                             int16_t& lastObj, bool& lastObjCulled) {
        RENDER_LOST(uint32_t submitted = 0);
        for (int ti = 0; ti < count; ti++) {
            const TriangleRef& ref = refs[ti];
            if (ref.objectIndex >= objects.size()) continue;
            GameObject* obj = objects[ref.objectIndex];
            if (!obj->isActive()) continue;
            if (obj->isDynamicMoved()) continue;  // Rendered in dynamic pass below
            if (obj->isSkinned()) continue;  // Rendered in skinned pass
            if (ref.triangleIndex >= obj->polyCount) continue;
            if (ref.objectIndex != lastObj) {
                lastObj = ref.objectIndex;
                BVHNode objBox;
                objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
                objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
                if (!frustum.testAABB(objBox)) {
                    lastObjCulled = true;
                    continue;
                }
                lastObjCulled = false;
                setupObjectTransform(obj, cameraPosition);
            }
            if (lastObjCulled) continue;
            if (ref.triangleIndex >= obj->polyCount) continue;  // streamed out
            RENDER_LOST(submitted++);
            processTriangle(obj->polygons[ref.triangleIndex], fogFarSZ, ot, balloc, obj->uvOffset);
        }
        RENDER_LOST(m_lost.tris += submitted);
    };

    ScreenRect full = {-512, -512, 832, 752};

    // Render a room's geometry, optionally clipped to a portal screen rect.
    // clipRect is the accumulated portal clip rectangle for portal-reached rooms,
    // or the fullscreen rect for the camera room and catch-all.
    auto renderRoom = [&](int ri, const ScreenRect& clipRect) {
        const RoomData& rm = rooms[ri];
        int16_t lastObj = -1;
        bool lastObjCulled = false;

        if (rm.cellCount > 0 && cells != nullptr) {
            // Only pay for cell-vs-screen projection when the clip rect is narrower
            // than fullscreen (i.e. room was reached through a portal chain).
            bool narrowClip = (clipRect.minX > -512 || clipRect.minY > -512 ||
                               clipRect.maxX < 832 || clipRect.maxY < 752);

            // Cell-based frustum culling: test each cell's AABB before processing its tris.
            for (int ci = 0; ci < rm.cellCount; ci++) {
                const RoomCell& cell = cells[rm.firstCell + ci];
                if (!frustum.testAABB(cell)) continue;

                // For portal-reached rooms, project cell AABB to screen and skip
                // cells that don't overlap the visible portal area.
                if (narrowClip) {
                    ScreenRect cellRect;
                    if (!projectAABBToScreen(cell.minX, cell.minY, cell.minZ,
                                            cell.maxX, cell.maxY, cell.maxZ,
                                            camX, camY, camZ, camRot, projH, cellRect)) continue;
                    ScreenRect clipped;
                    if (!intersectRect(clipRect, cellRect, clipped)) continue;
                }

                renderTriRefs(&roomTriRefs[cell.firstTriRef], cell.triRefCount,
                              lastObj, lastObjCulled);
            }
        } else {
            // Fallback: render all tris in room (no cell data available)
            renderTriRefs(&roomTriRefs[rm.firstTriRef], rm.triRefCount,
                          lastObj, lastObjCulled);
        }
    };

    // Always render catch-all room (geometry not assigned to any specific room)
    renderRoom(catchAllIdx, full);

    if (cameraRoom >= 0) {
        if (cameraRoom < 32) visited |= (1u << cameraRoom);
        stack[top++] = {cameraRoom, 0, full};
        while (top > 0) {
            Entry e = stack[--top];
            renderRoom(e.room, e.clip);
            if (e.depth >= 8) continue;  // Depth limit prevents infinite loops

            // Iterate portals connected to this room.
            // If per-room portal refs are available, use the indexed list (O(k) where k = neighbors).
            // Otherwise fall back to scanning all portals (O(N)).
            auto processPortal = [&](int p, int other) {
                if (other < 0 || other >= roomCount) return;
                if (other < 32 && (visited & (1u << other))) return;

                // Backface cull: skip portals that face away from the camera.
                {
                    int32_t dx = camX - portals[p].centerX;
                    int32_t dy = camY - portals[p].centerY;
                    int32_t dz = camZ - portals[p].centerZ;
                    int64_t dot = (int64_t)dx * portals[p].normalX +
                                  (int64_t)dy * portals[p].normalY +
                                  (int64_t)dz * portals[p].normalZ;
                    const int64_t BACKFACE_THRESHOLD = (int64_t)1024 * 4096;
                    if (portals[p].roomA == e.room) {
                        if (dot > BACKFACE_THRESHOLD) return;
                    } else {
                        if (dot < -BACKFACE_THRESHOLD) return;
                    }
                }

                // Frustum-cull the destination room's AABB.
                if (!isRoomPotentiallyVisible(rooms[other], camX, camY, camZ, camRot)) {
                    return;
                }

                // Project actual portal quad corners to screen.
                ScreenRect pr;
                if (!projectPortalRect(portals[p], camX, camY, camZ, camRot, projH, pr)) {
                    return;
                }
                ScreenRect isect;
                if (!intersectRect(e.clip, pr, isect)) {
                    return;
                }
                // Skip portals whose visible screen area is too small to matter.
                // Prevents rendering entire rooms through tiny distant slivers.
                if ((isect.maxX - isect.minX) < MIN_PORTAL_SCREEN_DIM ||
                    (isect.maxY - isect.minY) < MIN_PORTAL_SCREEN_DIM) return;

                // Project the destination room's AABB to screen and intersect with
                // the portal clip rect. This catches rooms whose geometry footprint
                // doesn't actually overlap the portal opening (e.g. a room off to
                // the side viewed through an oblique portal whose screen AABB is
                // misleadingly wide). Also tightens the clip rect for child portal
                // tests and cell culling.
                {
                    ScreenRect roomRect;
                    if (projectAABBToScreen(rooms[other].aabbMinX, rooms[other].aabbMinY,
                                            rooms[other].aabbMinZ, rooms[other].aabbMaxX,
                                            rooms[other].aabbMaxY, rooms[other].aabbMaxZ,
                                            camX, camY, camZ, camRot, projH, roomRect)) {
                        ScreenRect tighter;
                        if (!intersectRect(isect, roomRect, tighter)) return;
                        isect = tighter;
                        // Re-check minimum size after tightening
                        if ((isect.maxX - isect.minX) < MIN_PORTAL_SCREEN_DIM ||
                            (isect.maxY - isect.minY) < MIN_PORTAL_SCREEN_DIM) return;
                    }
                }

                if (other < 32) visited |= (1u << other);
                if (top < 64) stack[top++] = {other, e.depth + 1, isect};
            };

            if (roomPortalRefs && rooms[e.room].portalRefCount > 0) {
                // Per-room indexed portal list: only iterate portals touching this room.
                for (int pr = 0; pr < rooms[e.room].portalRefCount; pr++) {
                    const auto& ref = roomPortalRefs[rooms[e.room].firstPortalRef + pr];
                    processPortal(ref.portalIndex, ref.otherRoom);
                }
            } else {
                // Fallback: scan all portals to find ones connected to this room.
                for (int p = 0; p < portalCount; p++) {
                    int other = -1;
                    if (portals[p].roomA == e.room) other = portals[p].roomB;
                    else if (portals[p].roomB == e.room) other = portals[p].roomA;
                    else continue;
                    processPortal(p, other);
                }
            }
        }
    } else {
        // Camera room unknown - render ALL rooms as safety fallback.
        // This guarantees no geometry disappears, at the cost of no culling.
        for (int r = 0; r < roomCount; r++) if (r != catchAllIdx) renderRoom(r, full);
    }

#ifdef PSXSPLASH_ROOM_DEBUG
    // ================================================================
    // Debug overlay: render ALL room triangles as flat-color wireframe
    // on top of the scene. Each room gets a unique color. Brightness
    // modulates with distance so you can see depth. Triangles from
    // rooms that were NOT visited by the portal DFS are drawn dimmer.
    // Portal outlines and room AABB boxes are drawn as well.
    // ================================================================
    {
        static const psyqo::Color roomColors[] = {
            {.r = 255, .g = 50,  .b = 50},   // R0: red
            {.r = 50,  .g = 255, .b = 50},   // R1: green
            {.r = 50,  .g = 50,  .b = 255},  // R2: blue
            {.r = 255, .g = 255, .b = 50},   // R3: yellow
            {.r = 255, .g = 50,  .b = 255},  // R4: magenta
            {.r = 50,  .g = 255, .b = 255},  // R5: cyan
            {.r = 255, .g = 128, .b = 50},   // R6: orange
            {.r = 128, .g = 128, .b = 255},  // R7: lavender
        };
        constexpr int NUM_COLORS = sizeof(roomColors) / sizeof(roomColors[0]);

        // Room status bars at top of screen
        for (int r = 0; r < roomCount && r < 8; r++) {
            bool rendered = (visited & (1u << r)) != 0;
            bool isCamRoom = (r == cameraRoom);
            auto& tile = balloc.allocateFragment<psyqo::Prim::FastFill>();
            int16_t x = r * 18 + 2;
            tile.primitive.setColor(rendered ?
                roomColors[r % NUM_COLORS] : psyqo::Color{.r = 40, .g = 40, .b = 40});
            tile.primitive.rect = psyqo::Rect{
                .a = {.x = x, .y = (int16_t)2},
                .b = {.w = 14, .h = (int16_t)(isCamRoom ? 12 : 6)}
            };
            ot.insert(tile, 0);
        }

        // Render ALL room triangles as flat-colored tris on top.
        // Render triangles from visited rooms only, in per-room color.
        for (int ri = 0; ri < roomCount; ri++) {
            const RoomData& rm = rooms[ri];
            bool wasVisited = (ri < 32) ? ((visited & (1u << ri)) != 0) : false;
            bool isCatchAll = (ri == catchAllIdx);
            // Only draw rooms that were actually rendered (visited by portal DFS or catch-all)
            if (!wasVisited && !isCatchAll) continue;
            psyqo::Color baseColor = isCatchAll
                ? psyqo::Color{.r = 128, .g = 128, .b = 128}  // catch-all: gray
                : roomColors[ri % NUM_COLORS];

            for (int ti = 0; ti < rm.triRefCount; ti++) {
                const TriangleRef& ref = roomTriRefs[rm.firstTriRef + ti];
                if (ref.objectIndex >= objects.size()) continue;
                GameObject* obj = objects[ref.objectIndex];
                if (ref.triangleIndex >= obj->polyCount) continue;

                Tri& tri = obj->polygons[ref.triangleIndex];
                setupObjectTransform(obj, cameraPosition);

                // GTE transform
                writeSafe<PseudoRegister::V0>(tri.v0);
                writeSafe<PseudoRegister::V1>(tri.v1);
                writeSafe<PseudoRegister::V2>(tri.v2);
                Kernels::rtpt();

                uint32_t u0, u1, u2;
                read<Register::SZ1>(&u0);
                read<Register::SZ2>(&u1);
                read<Register::SZ3>(&u2);
                int32_t sz0 = (int32_t)u0, sz1 = (int32_t)u1, sz2 = (int32_t)u2;
                if (sz0 < 1 && sz1 < 1 && sz2 < 1) continue;
                int32_t zMax = eastl::max(eastl::max(sz0, sz1), sz2);
                if (zMax < 0 || zMax >= (int32_t)m_otSize) continue;

                psyqo::Vertex projected[3];
                read<Register::SXY0>(&projected[0].packed);
                read<Register::SXY1>(&projected[1].packed);
                read<Register::SXY2>(&projected[2].packed);
                if (isCompletelyOutside(projected[0], projected[1], projected[2])) continue;
                clampForRasterizer(projected[0]);
                clampForRasterizer(projected[1]);
                clampForRasterizer(projected[2]);

                // Depth-modulate brightness: near = full color, far = dimmer.
                // Also dim triangles from rooms that were NOT visited (culled).
                int32_t avgSZ = (sz0 + sz1 + sz2) / 3;
                int32_t bright = 4096 - (avgSZ * 4096 / (int32_t)m_otSize);
                if (bright < 512) bright = 512;
                if (bright > 4096) bright = 4096;

                psyqo::Color c = {
                    .r = (uint8_t)((baseColor.r * bright) >> 12),
                    .g = (uint8_t)((baseColor.g * bright) >> 12),
                    .b = (uint8_t)((baseColor.b * bright) >> 12),
                };

                auto& p = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                p.primitive.pointA = projected[0];
                p.primitive.pointB = projected[1];
                p.primitive.pointC = projected[2];
                p.primitive.setColorA(c);
                p.primitive.setColorB(c);
                p.primitive.setColorC(c);
                p.primitive.setSemiTrans();
                ot.insert(p, 0);
            }
        }

        // Portal outlines: project portal quad and draw edges as thin lines.
        for (int p = 0; p < portalCount; p++) {
            const PortalData& portal = portals[p];

            int32_t rwx = ((int32_t)portal.rightX * portal.halfW) >> 12;
            int32_t rwy = ((int32_t)portal.rightY * portal.halfW) >> 12;
            int32_t rwz = ((int32_t)portal.rightZ * portal.halfW) >> 12;
            int32_t uhx = ((int32_t)portal.upX * portal.halfH) >> 12;
            int32_t uhy = ((int32_t)portal.upY * portal.halfH) >> 12;
            int32_t uhz = ((int32_t)portal.upZ * portal.halfH) >> 12;

            int32_t cx = portal.centerX, cy = portal.centerY, cz = portal.centerZ;
            struct { int32_t wx, wy, wz; } corners[4] = {
                {cx + rwx + uhx, cy + rwy + uhy, cz + rwz + uhz},
                {cx - rwx + uhx, cy - rwy + uhy, cz - rwz + uhz},
                {cx - rwx - uhx, cy - rwy - uhy, cz - rwz - uhz},
                {cx + rwx - uhx, cy + rwy - uhy, cz + rwz - uhz},
            };

            int16_t sx[4], sy[4];
            bool vis[4];
            int visCount = 0;
            for (int i = 0; i < 4; i++) {
                int32_t vx, vy, vz;
                worldToCamera(corners[i].wx, corners[i].wy, corners[i].wz,
                              camX, camY, camZ, camRot, vx, vy, vz);
                vis[i] = projectToScreen(vx, vy, vz, projH, sx[i], sy[i]);
                if (vis[i]) visCount++;
            }
            if (visCount < 2) continue;

            bool portalActive = (visited & (1u << portal.roomA)) || (visited & (1u << portal.roomB));
            psyqo::Color lineColor = portalActive ?
                psyqo::Color{.r = 255, .g = 255, .b = 255} :
                psyqo::Color{.r = 100, .g = 80, .b = 0};

            for (int i = 0; i < 4; i++) {
                int j = (i + 1) % 4;
                if (!vis[i] || !vis[j]) continue;
                int16_t x0 = sx[i], y0 = sy[i], x1 = sx[j], y1 = sy[j];
                if (x0 < 0) x0 = 0; if (x0 > 319) x0 = 319;
                if (y0 < 0) y0 = 0; if (y0 > 239) y0 = 239;
                if (x1 < 0) x1 = 0; if (x1 > 319) x1 = 319;
                if (y1 < 0) y1 = 0; if (y1 > 239) y1 = 239;

                auto& tri = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                tri.primitive.pointA.x = x0; tri.primitive.pointA.y = y0;
                tri.primitive.pointB.x = x1; tri.primitive.pointB.y = y1;
                tri.primitive.pointC.x = x1; tri.primitive.pointC.y = (int16_t)(y1 + 1);
                tri.primitive.setColorA(lineColor);
                tri.primitive.setColorB(lineColor);
                tri.primitive.setColorC(lineColor);
                tri.primitive.setOpaque();
                ot.insert(tri, 0);
            }
        }

        // Room AABB outlines
        for (int r = 0; r < roomCount - 1 && r < 8; r++) {
            bool rendered = (visited & (1u << r)) != 0;
            psyqo::Color boxColor = rendered ?
                roomColors[r % NUM_COLORS] : psyqo::Color{.r = 60, .g = 60, .b = 60};

            const RoomData& rm = rooms[r];
            int32_t bmin[3] = {rm.aabbMinX, rm.aabbMinY, rm.aabbMinZ};
            int32_t bmax[3] = {rm.aabbMaxX, rm.aabbMaxY, rm.aabbMaxZ};

            int16_t csx[8], csy[8];
            bool cvis[8];
            int cvisCount = 0;
            for (int i = 0; i < 8; i++) {
                int32_t wx = (i & 1) ? bmax[0] : bmin[0];
                int32_t wy = (i & 2) ? bmax[1] : bmin[1];
                int32_t wz = (i & 4) ? bmax[2] : bmin[2];
                int32_t vx, vy, vz;
                worldToCamera(wx, wy, wz, camX, camY, camZ, camRot, vx, vy, vz);
                cvis[i] = projectToScreen(vx, vy, vz, projH, csx[i], csy[i]);
                if (cvis[i]) cvisCount++;
            }
            if (cvisCount < 2) continue;

            static const int edges[12][2] = {
                {0,1},{2,3},{4,5},{6,7},
                {0,2},{1,3},{4,6},{5,7},
                {0,4},{1,5},{2,6},{3,7},
            };
            for (int e = 0; e < 12; e++) {
                int a = edges[e][0], b = edges[e][1];
                if (!cvis[a] || !cvis[b]) continue;
                int16_t x0 = csx[a], y0 = csy[a], x1 = csx[b], y1 = csy[b];
                if (x0 < 0) x0 = 0; if (x0 > 319) x0 = 319;
                if (y0 < 0) y0 = 0; if (y0 > 239) y0 = 239;
                if (x1 < 0) x1 = 0; if (x1 > 319) x1 = 319;
                if (y1 < 0) y1 = 0; if (y1 > 239) y1 = 239;

                auto& tri = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
                tri.primitive.pointA.x = x0; tri.primitive.pointA.y = y0;
                tri.primitive.pointB.x = x1; tri.primitive.pointB.y = y1;
                tri.primitive.pointC.x = x1; tri.primitive.pointC.y = (int16_t)(y1 + 1);
                tri.primitive.setColorA(boxColor);
                tri.primitive.setColorB(boxColor);
                tri.primitive.setColorC(boxColor);
                tri.primitive.setOpaque();
                ot.insert(tri, 0);
            }
        }
    }
#endif

    // Render dynamically-moved objects that bypass room/portal culling
    for (size_t oi = 0; oi < objects.size(); oi++) {
        GameObject* obj = objects[oi];
        if (!obj->isActive() || !obj->isDynamicMoved()) continue;
        if (obj->isSkinned()) continue;
        BVHNode objBox;
        objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
        objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
        if (!frustum.testAABB(objBox)) continue;
        setupObjectTransform(obj, cameraPosition);
        RENDER_LOST(m_lost.tris += obj->polyCount);
        for (int t = 0; t < obj->polyCount; t++) {
            processTriangle(obj->polygons[t], fogFarSZ, ot, balloc, obj->uvOffset);
        }
    }

#if PSXSPLASH_FEATURE_SKIN
    renderSkinnedObjects(objects, cameraPosition, fogFarSZ, ot, balloc, &frustum);
#endif
#if PSXSPLASH_FEATURE_SHADOWS
    renderShadows(objects, cameraPosition, fogFarSZ, ot, balloc);
#endif

    // Sprites go in before the UI: both head-insert, so inserting the UI
    // later is what keeps it on top within its band.
#if PSXSPLASH_FEATURE_SPRITES
    if (m_tileSystem) m_tileSystem->renderOT(ot, balloc);
    if (m_spriteSystem) m_spriteSystem->renderOT(ot, balloc);
#endif
#if PSXSPLASH_FEATURE_UI
    if (m_uiSystem) m_uiSystem->renderOT(m_gpu, ot, balloc);
#endif
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderOT(ot, balloc);
#endif
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().renderOT(ot, balloc);
#endif
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    // Last insert into the far bucket, so the CLUT uploads precede every primitive of the frame.
    if (m_paletteCycler) m_paletteCycler->emit(ot, balloc, int32_t(m_otSize) - 1);
    m_gpu.chain(clear); m_gpu.chain(ot);
#if PSXSPLASH_FEATURE_UI
    if (m_uiSystem) m_uiSystem->renderText(m_gpu);
#endif
#ifdef PSXSPLASH_MEMOVERLAY
    if (m_memOverlay) m_memOverlay->renderText(m_gpu);
#endif
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().renderText(m_gpu);
#endif
    reportPeaks();
    m_frameCount++;
}

#if PSXSPLASH_FEATURE_SKIN
// ============================================================================
// Skinned mesh rendering - per-vertex bone transforms via rtps()
// ============================================================================

void psxsplash::Renderer::renderSkinnedObjects(
    eastl::vector<GameObject*>& objects,
    const psyqo::Vec3& cameraPosition,
    int32_t fogFarSZ,
    OT& ot,
    Balloc& balloc,
    const Frustum* frustum) {

    if (!m_skinSets || !m_skinStates || m_skinCount == 0) return;

#if PSXSPLASH_FEATURE_LIGHTS
    // Skinned meshes take no point lights; the shared leaf would apply the
    // last static object's.
    m_objLights.count = 0;
#endif
    // A triangle with a corner nearer than this goes to processTriangle: the
    // GTE's divide overflow bound, or the affine-warp split's reach.
    const int32_t nearOrSplitZ = eastl::max(m_nearSZ, m_splitZ);
    psyqo::Matrix33 identity = {};
    identity.vs[0].x.value = identity.vs[1].y.value = identity.vs[2].z.value = 4096;

    static psyqo::Matrix33 composedRots[SKINMESH_MAX_BONES];
    static psyqo::Vec3 composedTrans[SKINMESH_MAX_BONES];
    static BakedBoneMatrix lerpedBones[SKINMESH_MAX_BONES];

    for (int si = 0; si < m_skinCount; si++) {
        const SkinAnimSet& animSet = m_skinSets[si];
        const SkinAnimState& animState = m_skinStates[si];

        if (animSet.gameObjectIndex >= objects.size()) continue;
        GameObject* obj = objects[animSet.gameObjectIndex];
        if (!obj->isActive()) continue;
        if (animSet.clipCount == 0) continue;

        // Frustum cull using object AABB
        if (frustum) {
            BVHNode objBox;
            objBox.minX = obj->aabbMinX; objBox.minY = obj->aabbMinY; objBox.minZ = obj->aabbMinZ;
            objBox.maxX = obj->aabbMaxX; objBox.maxY = obj->aabbMaxY; objBox.maxZ = obj->aabbMaxZ;
            if (!frustum->testAABB(objBox)) continue;
        }

        const BakedBoneMatrix* boneMatricesA;
        const BakedBoneMatrix* boneMatricesB;
        uint16_t sf;
        if (!SkinMesh_CurrentFrames(animSet, animState, &boneMatricesA, &boneMatricesB, &sf)) continue;
        const BakedBoneMatrix* boneMatrices = boneMatricesA;

        // Interpolate between frames
        if (boneMatricesB) {
            for (int bi = 0; bi < animSet.boneCount && bi < SKINMESH_MAX_BONES; bi++) {
                const BakedBoneMatrix& bA = boneMatricesA[bi];
                const BakedBoneMatrix& bB = boneMatricesB[bi];
                BakedBoneMatrix& out = lerpedBones[bi];
                for (int k = 0; k < 9; k++) {
                    int32_t a = bA.r[k], b = bB.r[k];
                    out.r[k] = (int16_t)(a + (((b - a) * sf) >> 12));
                }
                for (int k = 0; k < 3; k++) {
                    int32_t a = bA.t[k], b = bB.t[k];
                    out.t[k] = (int16_t)(a + (((b - a) * sf) >> 12));
                }
            }
            boneMatrices = lerpedBones;
        }

        // Compose camera x object rotation. Reversed arguments, see setupObjectTransform.
        psyqo::Matrix33 camObjRot;
        psyqo::GteMath::multiplyMatrix33(obj->rotation, m_currentCamera->GetRotation(), &camObjRot);

        // Compute camera-space object position
        ::clear<Register::TRX, Safe>();
        ::clear<Register::TRY, Safe>();
        ::clear<Register::TRZ, Safe>();
        writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
        writeSafe<PseudoRegister::V0>(obj->position);
        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
        psyqo::Vec3 objCamPos = readSafe<PseudoRegister::SV>();
        objCamPos.x += cameraPosition.x;
        objCamPos.y += cameraPosition.y;
        objCamPos.z += cameraPosition.z;

        // Pre-compose per-bone rotation matrices and translations
        for (int bi = 0; bi < animSet.boneCount && bi < SKINMESH_MAX_BONES; bi++) {
            const BakedBoneMatrix& bm = boneMatrices[bi];

            psyqo::Matrix33 boneRot;
            boneRot.vs[0].x.value = bm.r[0]; boneRot.vs[0].y.value = bm.r[1]; boneRot.vs[0].z.value = bm.r[2];
            boneRot.vs[1].x.value = bm.r[3]; boneRot.vs[1].y.value = bm.r[4]; boneRot.vs[1].z.value = bm.r[5];
            boneRot.vs[2].x.value = bm.r[6]; boneRot.vs[2].y.value = bm.r[7]; boneRot.vs[2].z.value = bm.r[8];

            // composedRots[bi] = camObjRot x boneRot. Reversed arguments, see above.
            psyqo::GteMath::multiplyMatrix33(boneRot, camObjRot, &composedRots[bi]);

            // composedTrans[bi] = objCamPos + camObjRot x boneTrans
            psyqo::Vec3 boneTrans;
            boneTrans.x.value = bm.t[0]; boneTrans.y.value = bm.t[1]; boneTrans.z.value = bm.t[2];

            writeSafe<PseudoRegister::Rotation>(camObjRot);
            ::clear<Register::TRX, Safe>();
            ::clear<Register::TRY, Safe>();
            ::clear<Register::TRZ, Safe>();
            writeSafe<PseudoRegister::V0>(boneTrans);
            Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
            psyqo::Vec3 rotatedTrans = readSafe<PseudoRegister::SV>();

            composedTrans[bi].x.value = objCamPos.x.value + rotatedTrans.x.value;
            composedTrans[bi].y.value = objCamPos.y.value + rotatedTrans.y.value;
            composedTrans[bi].z.value = objCamPos.z.value + rotatedTrans.z.value;
        }

        const uint8_t* boneIdx = animSet.boneIndices;
        RENDER_LOST(m_lost.tris += animSet.polyCount);
        for (int ti = 0; ti < animSet.polyCount; ti++) {
            Tri& tri = animSet.polygons[ti];

            uint8_t bA = boneIdx ? boneIdx[ti * 3 + 0] : 0;
            uint8_t bB = boneIdx ? boneIdx[ti * 3 + 1] : 0;
            uint8_t bC = boneIdx ? boneIdx[ti * 3 + 2] : 0;
            if (bA >= animSet.boneCount) bA = 0;
            if (bB >= animSet.boneCount) bB = 0;
            if (bC >= animSet.boneCount) bC = 0;

            // Per-vertex rtps (3 calls instead of rtpt) due to different bone matrices
            psyqo::Vertex projected[3];
            uint32_t szRaw[3];
            const uint8_t bones[3] = {bA, bB, bC};
            const psyqo::GTE::PackedVec3* corners[3] = {&tri.v0, &tri.v1, &tri.v2};
            for (int k = 0; k < 3; k++) {
                writeSafe<PseudoRegister::Rotation>(composedRots[bones[k]]);
                writeSafe<PseudoRegister::Translation>(composedTrans[bones[k]]);
                writeSafe<PseudoRegister::V0>(*corners[k]);
                Kernels::rtps();
                read<Register::SZ3>(&szRaw[k]);
                read<Register::SXY2>(&projected[k].packed);
            }

            int32_t sz0 = (int32_t)szRaw[0], sz1 = (int32_t)szRaw[1], sz2 = (int32_t)szRaw[2];

            // All behind camera -> invisible
            if (sz0 <= 0 && sz1 <= 0 && sz2 <= 0) continue;
            // Beyond fog wall -> invisible
            if (fogFarSZ > 0 && sz0 > fogFarSZ && sz1 > fogFarSZ && sz2 > fogFarSZ) continue;

            const int32_t minZ = eastl::min(eastl::min(sz0, sz1), sz2);
            // A corner nearer than H/2 (where the GTE's SXY is wrong), an edge
            // the affine-warp split may take, or a corner off the rasterizer's
            // safe box: the static path's clipper and splitter handle it. They
            // take a triangle under one matrix, so this one is restated in view
            // space: RT the identity, TR the first corner's view position, the
            // corners their offsets from it. RTPT then gives each corner the
            // same MAC1-3, SZ and SXY its own bone's RTPS did.
            if ((minZ < nearOrSplitZ) | outsideRasterizerRange(projected[0]) | outsideRasterizerRange(projected[1]) |
                outsideRasterizerRange(projected[2])) {
                // Each corner's view position, RT*V + TR of its own bone,
                // unsaturated: the MAC1-3 its RTPS computed, again (read here
                // and not after each RTPS, so the other triangles pay nothing).
                int32_t view[3][3];
                for (int k = 0; k < 3; k++) {
                    writeSafe<PseudoRegister::Rotation>(composedRots[bones[k]]);
                    writeSafe<PseudoRegister::Translation>(composedTrans[bones[k]]);
                    writeSafe<PseudoRegister::V0>(*corners[k]);
                    Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
                    view[k][0] = (int32_t)readRaw<Register::MAC1, Safe>();
                    view[k][1] = (int32_t)readRaw<Register::MAC2, Safe>();
                    view[k][2] = (int32_t)readRaw<Register::MAC3, Safe>();
                }
                int16_t d[3][3];
                bool fits = true;
                for (int k = 0; k < 3; k++) {
                    for (int a = 0; a < 3; a++) {
                        const int32_t v = view[k][a] - view[0][a];
                        fits &= v >= -0x8000 && v < 0x8000;
                        d[k][a] = (int16_t)v;
                    }
                }
                // An edge longer than 16 bits in view space, which a skinned
                // triangle never has unless its bones tear it apart: dropped.
                if (!fits) {
                    RENDER_LOST(m_lost.skinNear++);
                    continue;
                }
                // Restated in place and put back after, not copied: a fourth
                // Tri copy made GCC outline Tri's copy constructor, a call per
                // piece in clipTriangle and per child in splitTriangle (the
                // courtyard, with no skinned mesh, 280 us a frame slower).
                const psyqo::GTE::PackedVec3 m0 = tri.v0, m1 = tri.v1, m2 = tri.v2;
                const uint16_t flags = tri.flags;
                psyqo::GTE::PackedVec3* out[3] = {&tri.v0, &tri.v1, &tri.v2};
                for (int k = 0; k < 3; k++) {
                    out[k]->x.value = d[k][0];
                    out[k]->y.value = d[k][1];
                    out[k]->z.value = d[k][2];
                }
                // Skinned triangles always took the object's UV offset.
                tri.flags = flags | ANIM_FLAG;
                writeSafe<PseudoRegister::Rotation>(identity);
                psyqo::Vec3 tr;
                tr.x.value = view[0][0]; tr.y.value = view[0][1]; tr.z.value = view[0][2];
                writeSafe<PseudoRegister::Translation>(tr);
                processTriangle(tri, fogFarSZ, ot, balloc, obj->uvOffset);
                tri.v0 = m0; tri.v1 = m1; tri.v2 = m2;
                tri.flags = flags;
                continue;
            }

            // Wholly past H/2 and inside the safe box: what processTriangle
            // does with such a triangle, on the RTPS output already here.
            int32_t zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
            if (zIndex > m_peakZ) m_peakZ = zIndex;
            // Past the table entirely -> dropped; straddling it -> the insert clamps it to the last bucket.
            if (minZ >= (int32_t)m_otSize) {
                RENDER_LOST(m_lost.ot++);
                continue;
            }
            if (zIndex < WORLD_DEPTH_MIN) zIndex = WORLD_DEPTH_MIN;

            // Off-screen reject
            if (isCompletelyOutside(projected[0], projected[1], projected[2])) continue;

            // Backface cull (SXY FIFO holds A,B,C after 3 sequential rtps calls)
            Kernels::nclip();
            int32_t mac0 = 0;
            read<Register::MAC0>(reinterpret_cast<uint32_t*>(&mac0));
            if (mac0 <= 0) continue;

            const int32_t sz[3] = {sz0, sz1, sz2};
            emitTriangle(tri, projected[0], projected[1], projected[2], sz, zIndex, fogFarSZ, ot, balloc, obj->uvOffset);
        }
    }
}
#endif

void psxsplash::Renderer::VramUpload(const uint16_t* imageData, int16_t posX,
                                     int16_t posY, int16_t width, int16_t height) {
    psyqo::Rect uploadRect{.a = {.x = posX, .y = posY}, .b = {width, height}};
    m_gpu.uploadToVRAM(imageData, uploadRect);
}
