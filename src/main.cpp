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
#include "memorycardmanager.hh"
#include "sio1.hh"

#if defined(PSXSPLASH_NETTEST)
#include "nettest.hh"
extern "C" {
extern volatile uint32_t g_netTestDone;
extern volatile uint32_t g_netTestPassed;
extern volatile uint32_t g_netTestFailed;
}
#endif

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

#if defined(PSXSPLASH_SIO1_ECHO)
    // Raw SIO1 link bring-up test (enabled with SIO1ECHO=1). Runs instead of
    // the game loop; sends a heartbeat byte per frame and shows RX/TX counters.
    void sio1SelfTest();
#endif
#if defined(PSXSPLASH_NETTEST)
    // In-RAM protocol self-test (NETTEST=1). Runs once, shows PASS/FAIL.
    void netTest();
    bool m_netTestRan = false;
    psxsplash::NetTestResult m_netTestResult{};
#endif

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

    // Bring up the SIO0 bus for memory card access.
    psxsplash::MemoryCardManager::Get().prepare(gpu());

#if defined(PSXSPLASH_SIO1_ECHO)
    // SIO1 is separate hardware from the SIO0 bus above - no conflict.
    psxsplash::Sio1::Get().init();
#endif

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
            // FileLoader init failed - nothing we can do on PS1.
        })
        .run();
}

void MainScene::frame() {
#if defined(PSXSPLASH_SIO1_ECHO)
    sio1SelfTest();
    return;  // the self-test owns the frame; skip the game loop
#endif
#if defined(PSXSPLASH_NETTEST)
    netTest();
    return;  // the self-test owns the frame; skip the game loop
#endif

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

#if defined(PSXSPLASH_SIO1_ECHO)
/**
 * Raw SIO1 link self-test - and the probe that validates the RX strategy.
 *
 * Both ends send a saturating stream of an incrementing counter, and each end
 * checks the received sequence for gaps. Because the payload is a known
 * sequence, a gap is unambiguous proof of DROPPED BYTES and tells us exactly how
 * many, which is the one thing byte counters alone cannot show.
 *
 * The stream is deliberately saturating. An earlier version sent one byte per
 * frame (~60 B/s) - that can never overrun an 8-byte FIFO, so it would report a
 * healthy link on hardware that is in fact losing ~96% of a real burst. A probe
 * that cannot fail is not a probe.
 *
 * Reading the result:
 *   lost == 0                  the RX path is keeping up.
 *   lost > 0 with errors > 0   STAT_OE is set: the hardware RX FIFO overran.
 *                              On real hardware in Polled mode this is expected
 *                              and is exactly why RxMode::Interrupt exists.
 *   mode                       the strategy Auto resolved to for this target.
 */
void MainScene::sio1SelfTest() {
    auto& sio = psxsplash::Sio1::Get();
    sio.poll();

    // Saturate the link: keep the TX ring topped up rather than sending a token
    // byte. ~192 bytes/frame is what 115200 8N1 can actually carry at 60Hz, so
    // this drives the line at full rate and stresses the peer's RX exactly as a
    // 10-player snapshot burst would.
    static uint8_t txVal = 0;
    for (int i = 0; i < 192; i++) {
        if (!sio.writeByte(txVal)) break;  // TX ring full - stop, don't spin
        txVal++;
    }

    // Verify the received sequence. Every byte should be exactly one more than
    // the last; anything else is a gap, and the gap size is the loss count.
    static uint32_t lost = 0;
    static uint8_t expected = 0;
    static bool primed = false;
    uint8_t buf[256];
    uint32_t n;
    while ((n = sio.read(buf, sizeof(buf))) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (!primed) {
                primed = true;
            } else if (b != expected) {
                lost += static_cast<uint8_t>(b - expected);  // wraps: counts the gap
            }
            expected = static_cast<uint8_t>(b + 1);
        }
    }

    const psyqo::Color white = {{.r = 0xff, .g = 0xff, .b = 0xff}};
    const psyqo::Color red = {{.r = 0xff, .g = 0x40, .b = 0x40}};
    const psyqo::Color green = {{.r = 0x40, .g = 0xff, .b = 0x40}};

    const bool interrupt = sio.rxMode() == psxsplash::Sio1::RxMode::Interrupt;
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 8}}, white, "SIO1 LINK SELF-TEST");
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 24}}, white, "RX mode : %s",
                           interrupt ? "INTERRUPT (hardware)" : "POLLED (emulator)");
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 36}}, white, "TX bytes: %i", (int)sio.bytesSent());
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 48}}, white, "RX bytes: %i", (int)sio.bytesReceived());
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 60}}, lost ? red : green, "BYTES LOST: %i", (int)lost);
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 72}}, sio.serialErrors() ? red : white, "STAT_OE etc: %i",
                           (int)sio.serialErrors());
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 84}}, white, "RX ring ovf: %i", (int)sio.rxOverflows());
    // How close the ring came to filling. Non-zero overflow is a post-mortem;
    // this is the number that shows trouble building while there is still time.
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 96}}, white, "RX peak : %i / %i", (int)sio.rxHighWater(),
                           (int)sio.rxCapacity());
    if (interrupt) {
        // Compare against RX bytes: this should be roughly bytes/8 with the FIFO
        // threshold at 8. Approaching 1:1 means the threshold has regressed to
        // per-byte interrupts, which alone can cost the console most of its CPU.
        app.m_font.chainprintf(gpu(), {{.x = 8, .y = 108}}, white, "RX IRQs : %i", (int)sio.rxInterrupts());
    }

    gpu().pumpCallbacks();
}
#endif

#if defined(PSXSPLASH_NETTEST)
void MainScene::netTest() {
    if (!m_netTestRan) {
        m_netTestResult = psxsplash::runNetLinkSelfTest();
        m_netTestRan = true;
        g_netTestPassed = (uint32_t)m_netTestResult.passed;
        g_netTestFailed = (uint32_t)m_netTestResult.failed;
        g_netTestDone = 0xC0DE0000u;  // signal to a headless harness
    }

    const psyqo::Color green = {{.r = 0x00, .g = 0xff, .b = 0x40}};
    const psyqo::Color red = {{.r = 0xff, .g = 0x30, .b = 0x30}};
    bool ok = (m_netTestResult.failed == 0);
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 8}}, ok ? green : red, "NETLINK SELF-TEST: %s",
                           ok ? "PASS" : "FAIL");
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 24}}, ok ? green : red, "passed: %i", m_netTestResult.passed);
    app.m_font.chainprintf(gpu(), {{.x = 8, .y = 36}}, ok ? green : red, "failed: %i", m_netTestResult.failed);

    gpu().pumpCallbacks();
}
#endif

int main() {
    // BEFORE run(): takeOverKernel() queues an initializer, and psyqo runs those
    // in Kernel::Internal::prepare() at the top of run(), ahead of our prepare().
    // Every driver that has to choose a dispatch path reads
    // Kernel::isKernelTakenOver(), which this sets immediately.
    //
    // See psxsplash::c_takeOverKernel for why. Short version: under the retail
    // BIOS every interrupt is dispatched by a handler that walks an event table
    // with interrupts disabled, and that blackout is long enough to overrun the
    // 8-byte SIO1 RX FIFO - which corrupts long serial frames only, and therefore
    // presents as a game-logic bug rather than a link one.
    if constexpr (psxsplash::c_takeOverKernel) {
        psyqo::Kernel::takeOverKernel();
    }

    // Turn a crash into a readable screen instead of a black one.
    //
    // On a retail PlayStation there is no other channel: Debug.Log goes to a BIOS
    // TTY that does not exist, so an unhandled exception is indistinguishable from
    // a dead console, a bad disc or a wrong cable. This prints the exception type,
    // the faulting address and every register. That is the difference between
    // "ReservedInstruction from 0x00005704" being a mystery and being a five
    // minute fix - one register would name whatever jumped there.
    //
    // LIMIT: it draws with the system font, which prepare() uploads to VRAM, so a
    // crash BEFORE that still shows nothing. It covers everything from the first
    // frame onward, which is where a game actually spends its life.
    psyqo::Kernel::installCrashHandler();

    return app.run();
}