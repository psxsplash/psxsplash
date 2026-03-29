#pragma once

#include <stdint.h>
#include <psyqo/font.hh>
#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include "uisystem.hh"

namespace psxsplash {

struct LoaderPackHeader {
    char magic[2];       // "LP"
    uint16_t version;    // 2
    uint8_t fontCount;
    uint8_t canvasCount; // always 1
    uint16_t resW;
    uint16_t resH;
    uint8_t atlasCount;  // texture atlases for UI images
    uint8_t clutCount;   // CLUTs for indexed-color images
    uint32_t tableOffset;
};
static_assert(sizeof(LoaderPackHeader) == 16, "LoaderPackHeader must be 16 bytes");

struct LoaderPackAtlas {
    uint32_t pixelDataOffset; // absolute offset in file
    uint16_t width, height;
    uint16_t x, y;            // VRAM position
};
static_assert(sizeof(LoaderPackAtlas) == 12, "LoaderPackAtlas must be 12 bytes");

struct LoaderPackClut {
    uint32_t clutDataOffset;  // absolute offset in file
    uint16_t clutX;           // VRAM X (in 16-pixel units × 16)
    uint16_t clutY;           // VRAM Y
    uint16_t length;          // number of palette entries
    uint16_t pad;
};
static_assert(sizeof(LoaderPackClut) == 12, "LoaderPackClut must be 12 bytes");


class LoadingScreen {
public:
    /// Try to load a loader pack from a file.
    /// Returns true if a loading screen was successfully loaded.
    /// @param gpu       GPU reference for rendering.
    /// @param systemFont  System font used if no custom font for text.
    /// @param sceneIndex  Scene index to derive the filename (scene_N.loading).
    bool load(psyqo::GPU& gpu, psyqo::Font<>& systemFont, int sceneIndex);

    /// Render all loading screen elements to BOTH framebuffers,
    /// then FREE all loaded data. After this, only updateProgress works.
    void renderInitialAndFree(psyqo::GPU& gpu);

    /// Update the progress bar to the given percentage (0-100).
    /// Redraws the progress bar rectangles in both framebuffers.
    /// Safe after data is freed — uses only cached layout values.
    void updateProgress(psyqo::GPU& gpu, uint8_t percent);

    /// Returns true if a loading screen was loaded (even after data freed).
    bool isActive() const { return m_active; }

private:
    /// Render a filled rectangle at an absolute VRAM position.
    void drawRect(psyqo::GPU& gpu, int16_t x, int16_t y, int16_t w, int16_t h,
                  uint8_t r, uint8_t g, uint8_t b);

    /// Render an image element (two textured triangles).
    /// Assumes DrawingOffset is already configured for the target buffer.
    void drawImage(psyqo::GPU& gpu, int handle, int16_t x, int16_t y,
                   int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b);

    /// Render custom-font text via sendPrimitive (TPage + Sprite per glyph).
    /// Assumes DrawingOffset is already configured for the target buffer.
    void drawCustomText(psyqo::GPU& gpu, int handle, int16_t x, int16_t y,
                        uint8_t r, uint8_t g, uint8_t b);

    /// Render ALL elements to a single framebuffer at the given VRAM Y offset.
    void renderToBuffer(psyqo::GPU& gpu, int16_t yOffset);

    /// Upload atlas/CLUT data to VRAM.
    void uploadTextures(psyqo::GPU& gpu);

    /// Find the "loading" progress bar element and cache its layout.
    void findProgressBar();

    uint8_t* m_data = nullptr;
    int m_dataSize = 0;
    psyqo::Font<>* m_font = nullptr;
    bool m_active = false;

    // Temporary UISystem to parse the loader pack's canvas/element data
    UISystem m_ui;

    // Cached layout for the "loading" progress bar (resolved at load time)
    bool m_hasProgressBar = false;
    int16_t m_barX = 0, m_barY = 0, m_barW = 0, m_barH = 0;
    uint8_t m_barFillR = 255, m_barFillG = 255, m_barFillB = 255;
    uint8_t m_barBgR = 0, m_barBgG = 0, m_barBgB = 0;

    // Resolution from the loader pack
    int16_t m_resW = 320, m_resH = 240;
};

} // namespace psxsplash
