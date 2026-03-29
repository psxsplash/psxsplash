#include <psyqo/advancedpad.hh>
#include <psyqo/application.hh>
#include <psyqo/fixed-point.hh>
#include <psyqo/font.hh>
#include <psyqo/gpu.hh>
#include <psyqo/scene.hh>
#include <psyqo/task.hh>
#include <psyqo/trigonometry.hh>

#include "renderer.hh"
#include "scenemanager.hh"
#include "fileloader.hh"

#if defined(LOADER_CDROM)
#include "fileloader_cdrom.hh"
#endif

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

    // Task queue for async FileLoader init (CD-ROM reset + ISO parse).
    // After init completes, loadScene() handles everything synchronously.
    psyqo::TaskQueue m_initQueue;
    bool m_ready = false;
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

    // Clear screen
    psyqo::Prim::FastFill ff(psyqo::Color{.r = 0, .g = 0, .b = 0});
    ff.rect = psyqo::Rect{0, 0, 320, 240};
    gpu().sendPrimitive(ff);
    ff.rect = psyqo::Rect{0, 256, 320, 240};
    gpu().sendPrimitive(ff);
    gpu().pumpCallbacks();

    // Let the active file-loader backend do any early setup.
    // CDRom: CDRomDevice::prepare() must happen here.
    psxsplash::FileLoader::Get().prepare();

#if defined(LOADER_CDROM)
    // The CD-ROM backend needs a GPU pointer for LoadFileSync's spin loop.
    static_cast<psxsplash::FileLoaderCDRom&>(
        psxsplash::FileLoader::Get()).setGPU(&gpu());
#endif
}

void PSXSplash::createScene() {
    m_font.uploadSystemFont(gpu());
    psxsplash::SceneManager::SetFont(&m_font);
    pushScene(&mainScene);
}

void MainScene::start(StartReason reason) {
    // Initialise the FileLoader backend, then load scene 0 through
    // the same SceneManager::loadScene() path used for all transitions.
    //
    // For PCdrv the init task resolves synchronously so both steps
    // execute in one go.  For CD-ROM the init is async (drive reset +
    // ISO9660 parse) and yields to the main loop until complete.

    m_initQueue
        .startWith(psxsplash::FileLoader::Get().scheduleInit())
        .then([this](psyqo::TaskQueue::Task* task) {
            m_sceneManager.loadScene(gpu(), 0, /*isFirstScene=*/true);
            m_ready = true;
            task->resolve();
        })
        .butCatch([](psyqo::TaskQueue*) {
            // FileLoader init failed — nothing we can do on PS1.
        })
        .run();
}

void MainScene::frame() {
    // Don't run the game loop while FileLoader init is still executing
    // (only relevant for the async CD-ROM backend).
    if (!m_ready) return;

    uint32_t beginFrame = gpu().now();
    auto currentFrameCounter = gpu().getFrameCount();
    auto deltaTime = currentFrameCounter - mainScene.m_lastFrameCounter;

    // Unlike the torus example, this DOES happen...
    if (deltaTime == 0) {
        return;
    }

    mainScene.m_lastFrameCounter = currentFrameCounter;
    
    m_sceneManager.GameTick(gpu());

    #if defined(PSXSPLASH_FPSOVERLAY)
    app.m_font.chainprintf(gpu(), {{.x = 2, .y = 2}}, {{.r = 0xff, .g = 0xff, .b = 0xff}}, "FPS: %i",
                           gpu().getRefreshRate() / deltaTime);
    #endif

    gpu().pumpCallbacks();
}

int main() { return app.run(); }