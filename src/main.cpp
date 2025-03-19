#include <stdint.h>

#include "EASTL/array.h"
#include "gameobject.hh"
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
    eastl::array<psxsplash::GameObject> objects;
};

PSXSplash psxSplash;
MainScene mainScene;

} // namespace

void PSXSplash::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
    m_renderer.initialize();
}

void PSXSplash::createScene() {
    m_font.uploadSystemFont(gpu());
    pushScene(&mainScene);
}

void MainScene::frame() { psxSplash.m_renderer.render(objects); }

int main() { return psxSplash.run(); }
