#include <psyqo/advancedpad.hh>
#include <psyqo/application.hh>
#include <psyqo/fixed-point.hh>
#include <psyqo/font.hh>
#include <psyqo/gpu.hh>
#include <psyqo/scene.hh>
#include <psyqo/trigonometry.hh>

#include "renderer.hh"
#include "scenemanager.hh"

// Data from the splashpack
extern uint8_t _binary_output_bin_start[];

namespace {

class PSXSplash final : public psyqo::Application {
    void prepare() override;
    void createScene() override;

  public:
    psyqo::Font<> m_font;
};

class MainScene final : public psyqo::Scene {
    void frame() override;
    void start(StartReason reason) override;

    uint32_t m_lastFrameCounter;

    psxsplash::SceneManager m_sceneManager;
};

PSXSplash app;
MainScene mainScene;

}  // namespace

void PSXSplash::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);

    // Initialize the Renderer singleton
    psxsplash::Renderer::Init(gpu());
}

void PSXSplash::createScene() {
    m_font.uploadSystemFont(gpu());
    pushScene(&mainScene);
}

void MainScene::start(StartReason reason) { m_sceneManager.InitializeScene(_binary_output_bin_start); }

void MainScene::frame() {
    uint32_t beginFrame = gpu().now();
    auto currentFrameCounter = gpu().getFrameCount();
    auto deltaTime = currentFrameCounter - mainScene.m_lastFrameCounter;

    // Unlike the torus example, this DOES happen...
    if (deltaTime == 0) {
        return;
    }

    mainScene.m_lastFrameCounter = currentFrameCounter;

    m_sceneManager.GameTick();

    app.m_font.chainprintf(gpu(), {{.x = 2, .y = 2}}, {{.r = 0xff, .g = 0xff, .b = 0xff}}, "FPS: %i",
                           gpu().getRefreshRate() / deltaTime);

    gpu().pumpCallbacks();
    uint32_t endFrame = gpu().now();
    uint32_t spent = endFrame - beginFrame;
}

int main() { return app.run(); }