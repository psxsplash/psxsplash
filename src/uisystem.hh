#pragma once

#include <stdint.h>
#include "vram_config.h"
#include "renderer.hh"
#include <psyqo/font.hh>
#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/triangles.hh>

namespace psxsplash {

static constexpr int UI_MAX_CANVASES  = 16;
static constexpr int UI_MAX_ELEMENTS  = 128;
static constexpr int UI_TEXT_BUF      = 64;
static constexpr int UI_MAX_FONTS     = 4;  // 0 = system font, 1-3 = custom

enum class UIElementType : uint8_t {
    Image    = 0,
    Box      = 1,
    Text     = 2,
    Progress = 3
};

struct UIImageData {
    uint8_t  texpageX, texpageY;
    uint16_t clutX, clutY;
    uint8_t  u0, v0, u1, v1;
    uint8_t  bitDepth; // 0=4bit, 1=8bit, 2=16bit
};

struct UIProgressData {
    uint8_t bgR, bgG, bgB;
    uint8_t value; // 0-100, mutable
};

struct UITextData {
    uint8_t fontIndex; // 0 = system font, 1+ = custom font
};

struct UIElement {
    UIElementType type;
    bool          visible;
    const char*   name;        // points into splashpack data
    int16_t x, y, w, h;       // pixel rect / offset in PS1 screen space
    uint8_t anchorMinX, anchorMinY, anchorMaxX, anchorMaxY; // 8.8 fixed
    uint8_t colorR, colorG, colorB;
    union { UIImageData image; UIProgressData progress; UITextData textData; };
    char textBuf[UI_TEXT_BUF]; // UIText: mutable, starts with default
};

struct UICanvas {
    const char* name;
    bool        visible;
    uint8_t     sortOrder;
    UIElement*  elements;  // slice into m_elements pool
    uint8_t     elementCount;
};

/// Font descriptor loaded from the splashpack (for VRAM upload).
struct UIFontDesc {
    uint8_t  glyphW, glyphH;
    uint16_t vramX, vramY;
    uint16_t textureH;
    const uint8_t* pixelData; // raw 4bpp, points into splashpack
    uint32_t pixelDataSize;
    uint8_t  advanceWidths[96]; // per-char advance (ASCII 0x20-0x7F) in pixels
};

class UISystem {
public:
    void init(psyqo::Font<>& systemFont);

    /// Called from SplashPackLoader after splashpack is loaded.
    /// Parses font descriptors (before canvas data) and canvas/element tables.
    void loadFromSplashpack(uint8_t* data, uint16_t canvasCount,
                            uint8_t fontCount, uint32_t tableOffset);

    /// Upload custom font textures to VRAM.
    /// Must be called AFTER loadFromSplashpack and BEFORE first render.
    void uploadFonts(psyqo::GPU& gpu);

    void relocate(intptr_t delta);

    // Phase 1: Insert OT primitives for boxes, images, progress bars, and custom font text.
    // Called BEFORE gpu.chain(ot) from inside the renderer.
    void renderOT(psyqo::GPU& gpu,
                  psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                  psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);

    // Phase 2: Emit system font text via psyqo font chaining.
    // Called AFTER gpu.chain(ot).
    void renderText(psyqo::GPU& gpu);

    // Canvas API
    int  findCanvas(const char* name) const;
    void setCanvasVisible(int idx, bool v);
    bool isCanvasVisible(int idx) const;

    // Element API - returns flat handle into m_elements, or -1
    int  findElement(int canvasIdx, const char* name) const;
    void setElementVisible(int handle, bool v);
    bool isElementVisible(int handle) const;
    void setText(int handle, const char* text);
    const char* getText(int handle) const;
    void setProgress(int handle, uint8_t value);
    void setColor(int handle, uint8_t r, uint8_t g, uint8_t b);
    void getColor(int handle, uint8_t& r, uint8_t& g, uint8_t& b) const;
    void setPosition(int handle, int16_t x, int16_t y);
    void getPosition(int handle, int16_t& x, int16_t& y) const;
    void setSize(int handle, int16_t w, int16_t h);
    void getSize(int handle, int16_t& w, int16_t& h) const;
    void setProgressColors(int handle, uint8_t bgR, uint8_t bgG, uint8_t bgB,
                           uint8_t fillR, uint8_t fillG, uint8_t fillB);
    uint8_t getProgress(int handle) const;
    UIElementType getElementType(int handle) const;
    int getCanvasElementCount(int canvasIdx) const;
    int getCanvasElementHandle(int canvasIdx, int elementIndex) const;
    void getProgressBgColor(int handle, uint8_t& r, uint8_t& g, uint8_t& b) const;
    int getCanvasCount() const;

    // Raw accessors for loading-screen direct rendering
    const UIImageData* getImageData(int handle) const;
    const UIFontDesc* getFontDesc(int fontIdx) const; // fontIdx = 0-based custom font index
    uint8_t getTextFontIndex(int handle) const;

private:
    psyqo::Font<>* m_systemFont = nullptr;

    UIFontDesc m_fontDescs[UI_MAX_FONTS - 1]; // descriptors from splashpack
    int m_fontCount = 0; // number of custom fonts (0-3)

    UICanvas  m_canvases[UI_MAX_CANVASES];
    UIElement m_elements[UI_MAX_ELEMENTS];
    int m_canvasCount  = 0;
    int m_elementCount = 0;

    // Pending text for system font only (custom fonts render in OT)
    struct PendingText { int16_t x, y; uint8_t r, g, b; const char* text; };
    PendingText m_pendingTexts[UI_MAX_ELEMENTS];
    int m_pendingTextCount = 0;

    /// Resolve which Font to use for system font (fontIndex 0).
    psyqo::FontBase* resolveFont(uint8_t fontIndex);

    void resolveLayout(const UIElement& el,
                       int16_t& outX, int16_t& outY,
                       int16_t& outW, int16_t& outH) const;

    void renderElement(UIElement& el,
                       psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                       psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);

    void renderProportionalText(int fontIdx, int16_t x, int16_t y,
                                uint8_t r, uint8_t g, uint8_t b,
                                const char* text,
                                psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                                psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);

    static psyqo::PrimPieces::TPageAttr makeTPage(const UIImageData& img);
};

} // namespace psxsplash
