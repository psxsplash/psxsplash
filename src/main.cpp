#include <stdint.h>

#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/scene.hh"

#include "renderer.hh"

namespace {

class PSXSplash final : public psyqo::Application {

    void prepare() override;
    void createScene() override;

  public:
    psyqo::Font<> m_font;
    psxsplash::Renderer m_renderer;
    PSXSplash() : m_renderer(gpu()) {}
};

class MainScene final : public psyqo::Scene {
    void frame() override;

    uint8_t m_anim = 0;
    bool m_direction = true;
};

PSXSplash psxSplash;
MainScene mainScene;

} 

void PSXSplash::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
}

void PSXSplash::createScene() {
    m_font.uploadSystemFont(gpu());
    pushScene(&mainScene);
}

void MainScene::frame() {
    if (m_anim == 0) {
        m_direction = true;
    } else if (m_anim == 255) {
        m_direction = false;
    }
    psyqo::Color bg{{.r = 0, .g = 64, .b = 91}};
    bg.r = m_anim;
    psxSplash.gpu().clear(bg);
    if (m_direction) {
        m_anim++;
    } else {
        m_anim--;
    }

    psyqo::Color c = {{.r = 255, .g = 255, .b = uint8_t(255 - m_anim)}};
    psxSplash.m_font.print(psxSplash.gpu(), "Hello World!", {{.x = 16, .y = 32}}, c);
}

int main() { return psxSplash.run(); }
