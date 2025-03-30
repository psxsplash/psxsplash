#include <stdint.h>

#include <cmath>
#include <cstdint>
#include <psyqo/advancedpad.hh>
#include <psyqo/application.hh>
#include <psyqo/fixed-point.hh>
#include <psyqo/font.hh>
#include <psyqo/gpu.hh>
#include <psyqo/scene.hh>
#include <psyqo/trigonometry.hh>

#include "camera.hh"
#include "gameobject.hh"
#include "renderer.hh"
#include "splashpack.hh"

// Data from the splashpack
extern uint8_t _binary_output_bin_start[];

namespace {

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;

class PSXSplash final : public psyqo::Application {
    void prepare() override;
    void createScene() override;

  public:
    psyqo::Font<> m_font;
    psyqo::AdvancedPad m_input;
};

class MainScene final : public psyqo::Scene {
    void frame() override;
    void start(StartReason reason) override;

    psxsplash::Camera m_mainCamera;
    psyqo::Angle camRotX, camRotY, camRotZ;

    eastl::vector<psxsplash::GameObject*> m_objects;
    psyqo::Trig<> m_trig;
    uint32_t m_lastFrameCounter;

    static constexpr psyqo::FixedPoint<12> moveSpeed = 0.01_fp;
    static constexpr psyqo::Angle rotSpeed = 0.01_pi;
};

PSXSplash psxSplash;
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
    psxsplash::Renderer::init(gpu());
}

void PSXSplash::createScene() {
    m_font.uploadSystemFont(gpu());
    m_input.initialize();
    pushScene(&mainScene);
}

void MainScene::start(StartReason reason) {
    m_objects = psxsplash::LoadSplashpack(_binary_output_bin_start);
    psxsplash::Renderer::getInstance().setCamera(m_mainCamera);
}

void MainScene::frame() {
    uint32_t beginFrame = gpu().now();
    auto currentFrameCounter = gpu().getFrameCount();
    auto deltaTime = currentFrameCounter - mainScene.m_lastFrameCounter;

    // Unlike the torus example, this DOES happen...
    if (deltaTime == 0) {
        return;
    }

    mainScene.m_lastFrameCounter = currentFrameCounter;

    auto& input = psxSplash.m_input;

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Right)) {
        m_mainCamera.moveX((m_trig.cos(camRotY) * moveSpeed * deltaTime));
        m_mainCamera.moveZ(-(m_trig.sin(camRotY) * moveSpeed * deltaTime));
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Left)) {
        m_mainCamera.moveX(-(m_trig.cos(camRotY) * moveSpeed * deltaTime));
        m_mainCamera.moveZ((m_trig.sin(camRotY) * moveSpeed * deltaTime));
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Up)) {
        m_mainCamera.moveX((m_trig.sin(camRotY) * m_trig.cos(camRotX)) * moveSpeed * deltaTime);
        m_mainCamera.moveY(-(m_trig.sin(camRotX) * moveSpeed));
        m_mainCamera.moveZ((m_trig.cos(camRotY) * m_trig.cos(camRotX)) * moveSpeed * deltaTime);
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Down)) {
        m_mainCamera.moveX(-((m_trig.sin(camRotY) * m_trig.cos(camRotX)) * moveSpeed * deltaTime));
        m_mainCamera.moveY((m_trig.sin(camRotX) * moveSpeed * deltaTime));
        m_mainCamera.moveZ(-((m_trig.cos(camRotY) * m_trig.cos(camRotX)) * moveSpeed * deltaTime));
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::R1)) {
        m_mainCamera.moveY(-moveSpeed * deltaTime);
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::L1)) {
        m_mainCamera.moveY(moveSpeed * deltaTime);
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Cross)) {
        camRotX -= rotSpeed * deltaTime;
        m_mainCamera.setRotation(camRotX, camRotY, camRotZ);
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Triangle)) {
        camRotX += rotSpeed * deltaTime;
        m_mainCamera.setRotation(camRotX, camRotY, camRotZ);
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Circle)) {
        camRotY += rotSpeed * deltaTime;
        m_mainCamera.setRotation(camRotX, camRotY, camRotZ);
    }

    if (input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Square)) {
        camRotY -= rotSpeed * deltaTime;
        m_mainCamera.setRotation(camRotX, camRotY, camRotZ);
    }

    psxsplash::Renderer::getInstance().render(m_objects);

    psxSplash.m_font.chainprintf(gpu(), {{.x = 2, .y = 2}}, {{.r = 0xff, .g = 0xff, .b = 0xff}}, "FPS: %i",
                                 gpu().getRefreshRate() / deltaTime);
    gpu().pumpCallbacks();
    uint32_t endFrame = gpu().now();
    uint32_t spent = endFrame - beginFrame;
}

int main() { return psxSplash.run(); }
