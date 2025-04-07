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

#include "EASTL/algorithm.h"
#include "camera.hh"
#include "navmesh.hh"
#include "psyqo/vector.hh"
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
    psxsplash::SplashPackLoader m_loader;
    static constexpr uint8_t m_stickDeadzone = 0x30;

};

class MainScene final : public psyqo::Scene {
    void frame() override;
    void start(StartReason reason) override;

    psxsplash::Camera m_mainCamera;
    psyqo::Angle camRotX, camRotY, camRotZ;

    psyqo::Trig<> m_trig;
    uint32_t m_lastFrameCounter;

    static constexpr psyqo::FixedPoint<12> moveSpeed = 0.002_fp;
    static constexpr psyqo::Angle rotSpeed = 0.01_pi;
    bool m_sprinting = 0;
    static constexpr psyqo::FixedPoint<12> sprintSpeed = 0.003_fp;

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
    psxsplash::Renderer::Init(gpu());
}

void PSXSplash::createScene() {
    m_font.uploadSystemFont(gpu());
    m_input.initialize();
    pushScene(&mainScene);
}

void MainScene::start(StartReason reason) {
    psxSplash.m_loader.LoadSplashpack(_binary_output_bin_start);
    psxsplash::Renderer::GetInstance().SetCamera(m_mainCamera);
}

psyqo::FixedPoint<12> pheight = 0.0_fp;

void MainScene::frame() {
    uint32_t beginFrame = gpu().now();
    auto currentFrameCounter = gpu().getFrameCount();
    auto deltaTime = currentFrameCounter - mainScene.m_lastFrameCounter;

    // Unlike the torus example, this DOES happen...
    if (deltaTime == 0) {
        return;
    }

    mainScene.m_lastFrameCounter = currentFrameCounter;
    
    uint8_t rightX = psxSplash.m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 0);
    uint8_t rightY = psxSplash.m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 1);

    uint8_t leftX = psxSplash.m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 2);
    uint8_t leftY = psxSplash.m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 3);

    int16_t rightXOffset = (int16_t)rightX - 0x80;
    int16_t rightYOffset = (int16_t)rightY - 0x80;
    int16_t leftXOffset = (int16_t)leftX - 0x80;
    int16_t leftYOffset = (int16_t)leftY - 0x80;
    
    if(__builtin_abs(leftXOffset) < psxSplash.m_stickDeadzone && 
       __builtin_abs(leftYOffset) < psxSplash.m_stickDeadzone) {
        m_sprinting = false;
    }

    if(psxSplash.m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L3)) {
        m_sprinting = true;
    }

    psyqo::FixedPoint<12> speed = m_sprinting ? sprintSpeed : moveSpeed;

    if (__builtin_abs(rightXOffset) > psxSplash.m_stickDeadzone) {
        camRotY += (rightXOffset * rotSpeed * deltaTime) >> 7;  
    }
    if (__builtin_abs(rightYOffset) > psxSplash.m_stickDeadzone) {
        camRotX -= (rightYOffset * rotSpeed * deltaTime) >> 7;
        camRotX = eastl::clamp(camRotX, -0.5_pi, 0.5_pi);
    }
    m_mainCamera.SetRotation(camRotX, camRotY, camRotZ);

    if (__builtin_abs(leftYOffset) > psxSplash.m_stickDeadzone) {
        psyqo::FixedPoint<12> forward = -(leftYOffset * speed * deltaTime) >> 7;
        m_mainCamera.MoveX((m_trig.sin(camRotY) * forward));
        m_mainCamera.MoveZ((m_trig.cos(camRotY) * forward));
    }
    if (__builtin_abs(leftXOffset) > psxSplash.m_stickDeadzone) {
        psyqo::FixedPoint<12>  strafe = -(leftXOffset * speed * deltaTime) >> 7;
        m_mainCamera.MoveX(-(m_trig.cos(camRotY) * strafe));
        m_mainCamera.MoveZ((m_trig.sin(camRotY) * strafe));
    }

    if(psxSplash.m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L1)) {
        pheight += 0.01_fp;
    }
    if(psxSplash.m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::R1)) {
        pheight -= 0.01_fp;
    }

    psyqo::Vec3 adjustedPosition =
        psxsplash::ComputeNavmeshPosition(m_mainCamera.GetPosition(), *psxSplash.m_loader.navmeshes[0], -0.05_fp);
    m_mainCamera.SetPosition(adjustedPosition.x, adjustedPosition.y, adjustedPosition.z);

    psxsplash::Renderer::GetInstance().Render(psxSplash.m_loader.gameObjects);
    // psxsplash::Renderer::getInstance().renderNavmeshPreview(*psxSplash.m_loader.navmeshes[0], true);
    psxSplash.m_font.chainprintf(gpu(), {{.x = 2, .y = 2}}, {{.r = 0xff, .g = 0xff, .b = 0xff}}, "FPS: %i",
                                 gpu().getRefreshRate() / deltaTime);

    gpu().pumpCallbacks();
    uint32_t endFrame = gpu().now();
    uint32_t spent = endFrame - beginFrame;
}

int main() { return psxSplash.run(); }