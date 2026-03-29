#include "loadingscreen.hh"
#include "fileloader.hh"
#include <psyqo/kernel.hh>
#include "renderer.hh"

#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/primitives/sprites.hh>
#include <psyqo/primitives/control.hh>

namespace psxsplash {

static constexpr int16_t kBuffer1Y = 256;

// This file has duplicate code from UISystem... This is terrible...

// ────────────────────────────────────────────────────────────────
// Load
// ────────────────────────────────────────────────────────────────
bool LoadingScreen::load(psyqo::GPU& gpu, psyqo::Font<>& systemFont, int sceneIndex) {
    // Build filename using the active backend's naming convention
    char filename[32];
    FileLoader::BuildLoadingFilename(sceneIndex, filename, sizeof(filename));
    int fileSize = 0;
    uint8_t* data = FileLoader::Get().LoadFileSync(filename, fileSize);
    if (!data || fileSize < (int)sizeof(LoaderPackHeader)) {
        if (data) FileLoader::Get().FreeFile(data);
        return false;
    }

    auto* header = reinterpret_cast<const LoaderPackHeader*>(data);
    if (header->magic[0] != 'L' || header->magic[1] != 'P') {
        FileLoader::Get().FreeFile(data);
        return false;
    }

    m_data = data;
    m_dataSize = fileSize;
    m_font = &systemFont;
    m_active = true;
    m_resW = (int16_t)header->resW;
    m_resH = (int16_t)header->resH;

    // Upload texture atlases and CLUTs to VRAM (before setting DrawingOffset)
    uploadTextures(gpu);

    // Initialize UISystem with the system font and parse the loader pack
    m_ui.init(systemFont);
    m_ui.loadFromSplashpack(data, header->canvasCount, header->fontCount, header->tableOffset);
    m_ui.uploadFonts(gpu);

    // Ensure canvas 0 is visible
    if (m_ui.getCanvasCount() > 0) {
        m_ui.setCanvasVisible(0, true);
    }

    // Find the progress bar named "loading"
    findProgressBar();

    return true;
}

// ────────────────────────────────────────────────────────────────
// Upload atlas/CLUT data to VRAM (RAM → VRAM blit)
// ────────────────────────────────────────────────────────────────
void LoadingScreen::uploadTextures(psyqo::GPU& gpu) {
    auto* header = reinterpret_cast<const LoaderPackHeader*>(m_data);

    // Atlas and CLUT entries start right after the 16-byte header
    const uint8_t* ptr = m_data + sizeof(LoaderPackHeader);

    for (int ai = 0; ai < header->atlasCount; ai++) {
        auto* atlas = reinterpret_cast<const LoaderPackAtlas*>(ptr);
        ptr += sizeof(LoaderPackAtlas);

        if (atlas->pixelDataOffset != 0 && atlas->width > 0 && atlas->height > 0) {
            const uint16_t* pixels = reinterpret_cast<const uint16_t*>(m_data + atlas->pixelDataOffset);
            psyqo::Rect region{.a = {.x = (int16_t)atlas->x, .y = (int16_t)atlas->y},
                               .b = {(int16_t)atlas->width, (int16_t)atlas->height}};
            gpu.uploadToVRAM(pixels, region);
        }
    }

    for (int ci = 0; ci < header->clutCount; ci++) {
        auto* clut = reinterpret_cast<const LoaderPackClut*>(ptr);
        ptr += sizeof(LoaderPackClut);

        if (clut->clutDataOffset != 0 && clut->length > 0) {
            const uint16_t* palette = reinterpret_cast<const uint16_t*>(m_data + clut->clutDataOffset);
            psyqo::Rect region{.a = {.x = (int16_t)(clut->clutX * 16), .y = (int16_t)clut->clutY},
                               .b = {(int16_t)clut->length, 1}};
            gpu.uploadToVRAM(palette, region);
        }
    }
}

// ────────────────────────────────────────────────────────────────
// Find progress bar
// ────────────────────────────────────────────────────────────────
void LoadingScreen::findProgressBar() {
    m_hasProgressBar = false;

    int canvasCount = m_ui.getCanvasCount();
    for (int ci = 0; ci < canvasCount; ci++) {
        int handle = m_ui.findElement(ci, "loading");
        if (handle >= 0 && m_ui.getElementType(handle) == UIElementType::Progress) {
            m_hasProgressBar = true;

            m_ui.getPosition(handle, m_barX, m_barY);
            m_ui.getSize(handle, m_barW, m_barH);
            m_ui.getColor(handle, m_barFillR, m_barFillG, m_barFillB);
            m_ui.getProgressBgColor(handle, m_barBgR, m_barBgG, m_barBgB);
            break;
        }
    }
}

// ────────────────────────────────────────────────────────────────
// Draw a filled rectangle (immediate mode, no DMA chain).
// Coordinates are logical — DrawingOffset shifts them to the target buffer.
// ────────────────────────────────────────────────────────────────
void LoadingScreen::drawRect(psyqo::GPU& gpu, int16_t x, int16_t y,
                              int16_t w, int16_t h,
                              uint8_t r, uint8_t g, uint8_t b) {
    if (w <= 0 || h <= 0) return;
    psyqo::Prim::Rectangle rect;
    rect.setColor(psyqo::Color{.r = r, .g = g, .b = b});
    rect.position = {.x = x, .y = y};
    rect.size = {.x = w, .y = h};
    rect.setOpaque();
    gpu.sendPrimitive(rect);
}

// ────────────────────────────────────────────────────────────────
// Draw a textured image (two triangles, sendPrimitive).
// GouraudTexturedTriangle carries its own TPage attribute.
// DrawingOffset shifts logical coordinates to the correct framebuffer.
// ────────────────────────────────────────────────────────────────
void LoadingScreen::drawImage(psyqo::GPU& gpu, int handle,
                               int16_t x, int16_t y, int16_t w, int16_t h,
                               uint8_t r, uint8_t g, uint8_t b) {
    const UIImageData* img = m_ui.getImageData(handle);
    if (!img) return;

    // Build TPage attribute
    psyqo::PrimPieces::TPageAttr tpage;
    tpage.setPageX(img->texpageX);
    tpage.setPageY(img->texpageY);
    switch (img->bitDepth) {
    case 0: tpage.set(psyqo::Prim::TPageAttr::Tex4Bits); break;
    case 1: tpage.set(psyqo::Prim::TPageAttr::Tex8Bits); break;
    case 2: default: tpage.set(psyqo::Prim::TPageAttr::Tex16Bits); break;
    }
    tpage.setDithering(false);

    psyqo::PrimPieces::ClutIndex clut(img->clutX, img->clutY);
    psyqo::Color tint = {.r = r, .g = g, .b = b};

    // Triangle 0: top-left, top-right, bottom-left
    {
        psyqo::Prim::GouraudTexturedTriangle tri;
        tri.pointA.x = x;       tri.pointA.y = y;
        tri.pointB.x = x + w;   tri.pointB.y = y;
        tri.pointC.x = x;       tri.pointC.y = y + h;
        tri.uvA.u = img->u0;    tri.uvA.v = img->v0;
        tri.uvB.u = img->u1;    tri.uvB.v = img->v0;
        tri.uvC.u = img->u0;    tri.uvC.v = img->v1;
        tri.tpage = tpage;
        tri.clutIndex = clut;
        tri.setColorA(tint);
        tri.setColorB(tint);
        tri.setColorC(tint);
        tri.setOpaque();
        gpu.sendPrimitive(tri);
    }
    // Triangle 1: top-right, bottom-right, bottom-left
    {
        psyqo::Prim::GouraudTexturedTriangle tri;
        tri.pointA.x = x + w;   tri.pointA.y = y;
        tri.pointB.x = x + w;   tri.pointB.y = y + h;
        tri.pointC.x = x;       tri.pointC.y = y + h;
        tri.uvA.u = img->u1;    tri.uvA.v = img->v0;
        tri.uvB.u = img->u1;    tri.uvB.v = img->v1;
        tri.uvC.u = img->u0;    tri.uvC.v = img->v1;
        tri.tpage = tpage;
        tri.clutIndex = clut;
        tri.setColorA(tint);
        tri.setColorB(tint);
        tri.setColorC(tint);
        tri.setOpaque();
        gpu.sendPrimitive(tri);
    }
}

// ────────────────────────────────────────────────────────────────
// Draw custom-font text via sendPrimitive (TPage + Sprite per glyph).
// DrawingOffset shifts logical coordinates to the correct framebuffer.
// ────────────────────────────────────────────────────────────────
void LoadingScreen::drawCustomText(psyqo::GPU& gpu, int handle,
                                    int16_t x, int16_t y,
                                    uint8_t r, uint8_t g, uint8_t b) {
    uint8_t fontIdx = m_ui.getTextFontIndex(handle);
    if (fontIdx == 0) return; // system font, not custom
    const UIFontDesc* fd = m_ui.getFontDesc(fontIdx - 1); // 1-based → 0-based
    if (!fd) return;

    const char* text = m_ui.getText(handle);
    if (!text || !text[0]) return;

    // Set TPage for this font's texture page
    psyqo::Prim::TPage tpCmd;
    tpCmd.attr.setPageX(fd->vramX >> 6);
    tpCmd.attr.setPageY(fd->vramY >> 8);
    tpCmd.attr.set(psyqo::Prim::TPageAttr::Tex4Bits);
    tpCmd.attr.setDithering(false);
    gpu.sendPrimitive(tpCmd);

    // CLUT reference (same as renderProportionalText in UISystem)
    psyqo::Vertex clutPos = {{.x = (int16_t)fd->vramX, .y = (int16_t)fd->vramY}};
    psyqo::PrimPieces::ClutIndex clutIdx(clutPos);
    psyqo::Color color = {.r = r, .g = g, .b = b};

    int glyphsPerRow = 256 / fd->glyphW;
    uint8_t baseV = fd->vramY & 0xFF;

    int16_t cursorX = x;
    while (*text) {
        uint8_t c = (uint8_t)*text++;
        if (c < 32 || c > 127) c = '?';
        uint8_t charIdx = c - 32;
        uint8_t advance = fd->advanceWidths[charIdx];

        if (c == ' ') {
            cursorX += advance;
            continue;
        }

        int charRow = charIdx / glyphsPerRow;
        int charCol = charIdx % glyphsPerRow;
        uint8_t u = (uint8_t)(charCol * fd->glyphW);
        uint8_t v = (uint8_t)(baseV + charRow * fd->glyphH);

        int16_t spriteW = (advance > 0 && advance < fd->glyphW) ? (int16_t)advance : (int16_t)fd->glyphW;

        psyqo::Prim::Sprite sprite;
        sprite.setColor(color);
        sprite.position = {.x = cursorX, .y = y};
        sprite.size = {.x = spriteW, .y = (int16_t)fd->glyphH};
        psyqo::PrimPieces::TexInfo texInfo;
        texInfo.u = u;
        texInfo.v = v;
        texInfo.clut = clutIdx;
        sprite.texInfo = texInfo;
        gpu.sendPrimitive(sprite);

        cursorX += advance;
    }
}

// ────────────────────────────────────────────────────────────────
// Render ALL elements to ONE framebuffer at the given VRAM Y offset.
// Uses DrawingOffset so all draw functions use logical coordinates.
// ────────────────────────────────────────────────────────────────
void LoadingScreen::renderToBuffer(psyqo::GPU& gpu, int16_t yOffset) {
    // Configure GPU drawing area for this framebuffer
    gpu.sendPrimitive(psyqo::Prim::DrawingAreaStart(psyqo::Vertex{{.x = 0, .y = yOffset}}));
    gpu.sendPrimitive(psyqo::Prim::DrawingAreaEnd(psyqo::Vertex{{.x = m_resW, .y = (int16_t)(yOffset + m_resH)}}));
    gpu.sendPrimitive(psyqo::Prim::DrawingOffset(psyqo::Vertex{{.x = 0, .y = yOffset}}));

    // Clear this buffer to black (Rectangle is shifted by DrawingOffset)
    drawRect(gpu, 0, 0, m_resW, m_resH, 0, 0, 0);

    int canvasCount = m_ui.getCanvasCount();
    for (int ci = 0; ci < canvasCount; ci++) {
        if (!m_ui.isCanvasVisible(ci)) continue;
        int elemCount = m_ui.getCanvasElementCount(ci);

        for (int ei = 0; ei < elemCount; ei++) {
            int handle = m_ui.getCanvasElementHandle(ci, ei);
            if (handle < 0 || !m_ui.isElementVisible(handle)) continue;

            UIElementType type = m_ui.getElementType(handle);
            int16_t x, y, w, h;
            m_ui.getPosition(handle, x, y);
            m_ui.getSize(handle, w, h);
            uint8_t r, g, b;
            m_ui.getColor(handle, r, g, b);

            switch (type) {
            case UIElementType::Box:
                drawRect(gpu, x, y, w, h, r, g, b);
                break;

            case UIElementType::Progress: {
                uint8_t bgr, bgg, bgb;
                m_ui.getProgressBgColor(handle, bgr, bgg, bgb);
                drawRect(gpu, x, y, w, h, bgr, bgg, bgb);

                uint8_t val = m_ui.getProgress(handle);
                int fillW = (int)val * w / 100;
                if (fillW > 0)
                    drawRect(gpu, x, y, (int16_t)fillW, h, r, g, b);
                break;
            }

            case UIElementType::Image:
                drawImage(gpu, handle, x, y, w, h, r, g, b);
                break;

            case UIElementType::Text: {
                uint8_t fontIdx = m_ui.getTextFontIndex(handle);
                if (fontIdx > 0) {
                    drawCustomText(gpu, handle, x, y, r, g, b);
                } else if (m_font) {
                    m_font->print(gpu, m_ui.getText(handle),
                        {{.x = x, .y = y}},
                        {{.r = r, .g = g, .b = b}});
                }
                break;
            }
            }
        }
    }
}

// ────────────────────────────────────────────────────────────────
// Render to both framebuffers, then FREE all loaded data
// ────────────────────────────────────────────────────────────────
void LoadingScreen::renderInitialAndFree(psyqo::GPU& gpu) {
    if (!m_data) return;

    // Render to framebuffer 0 (Y = 0)
    renderToBuffer(gpu, 0);
    gpu.pumpCallbacks();

    // Render to framebuffer 1 (Y = 256 — psyqo's hardcoded buffer-1 offset)
    renderToBuffer(gpu, kBuffer1Y);
    gpu.pumpCallbacks();

    // Restore normal scissor for the active framebuffer
    gpu.enableScissor();

    // FREE all loaded data — the splashpack needs this memory
    FileLoader::Get().FreeFile(m_data);
    m_data = nullptr;
    m_dataSize = 0;
}

// ────────────────────────────────────────────────────────────────
// Update progress bar in BOTH framebuffers
// ────────────────────────────────────────────────────────────────
void LoadingScreen::updateProgress(psyqo::GPU& gpu, uint8_t percent) {
    if (!m_hasProgressBar || !m_active) return;
    if (percent > 100) percent = 100;

    int fillW = (int)percent * m_barW / 100;

    // Draw into both framebuffers using DrawingOffset
    for (int buf = 0; buf < 2; buf++) {
        int16_t yOff = (buf == 0) ? 0 : kBuffer1Y;

        gpu.sendPrimitive(psyqo::Prim::DrawingAreaStart(psyqo::Vertex{{.x = 0, .y = yOff}}));
        gpu.sendPrimitive(psyqo::Prim::DrawingAreaEnd(psyqo::Vertex{{.x = m_resW, .y = (int16_t)(yOff + m_resH)}}));
        gpu.sendPrimitive(psyqo::Prim::DrawingOffset(psyqo::Vertex{{.x = 0, .y = yOff}}));

        // Background
        drawRect(gpu, m_barX, m_barY, m_barW, m_barH,
                 m_barBgR, m_barBgG, m_barBgB);

        // Fill
        if (fillW > 0) {
            drawRect(gpu, m_barX, m_barY, (int16_t)fillW, m_barH,
                     m_barFillR, m_barFillG, m_barFillB);
        }
    }

    // Restore normal scissor
    gpu.enableScissor();
    gpu.pumpCallbacks();
}

} // namespace psxsplash
