#pragma once

#include <stdint.h>
#include <psyqo/task.hh>

namespace psxsplash {

/**
 * FileLoader — abstract interface for loading files on PS1.
 *
 * Two compile-time backends:
 *   - FileLoaderPCdrv: PCdrv protocol (emulator break instructions OR SIO1 serial)
 *   - FileLoaderCDRom: CD-ROM via psyqo CDRomDevice + ISO9660Parser
 *
 * Build with LOADER=pcdrv (default) or LOADER=cdrom to select the backend.
 *
 * Both backends expose the same task-based API following the psyqo TaskQueue
 * pattern (see nugget/psyqo/examples/task-demo). For PCdrv the tasks resolve
 * synchronously; for CD-ROM they chain real async hardware I/O.
 *
 * The active backend singleton is accessed through FileLoader::Get().
 */
class FileLoader {
  public:
    virtual ~FileLoader() = default;

    /**
     * Called once from Application::prepare() before any GPU work.
     * CDRom backend uses this to call CDRomDevice::prepare().
     * PCdrv backend is a no-op.
     */
    virtual void prepare() {}

    /**
     * Returns a Task that initialises the loader.
     *
     * PCdrv: calls pcdrv_sio1_init() + pcdrv_init(), resolves immediately.
     * CDRom: chains CDRomDevice::scheduleReset + ISO9660Parser::scheduleInitialize.
     */
    virtual psyqo::TaskQueue::Task scheduleInit() = 0;

    /**
     * Returns a Task that loads a file.
     *
     * On resolve, *outBuffer points to the loaded data (caller owns it)
     * and *outSize contains the size in bytes.
     * On reject, *outBuffer == nullptr and *outSize == 0.
     *
     * PCdrv filenames: relative paths like "scene_0.splashpack".
     * CDRom filenames: ISO9660 names like "SCENE_0.SPK;1".
     *
     * Use BuildSceneFilename / BuildLoadingFilename helpers to get the
     * correct filename for the active backend.
     */
    virtual psyqo::TaskQueue::Task scheduleLoadFile(
        const char* filename, uint8_t*& outBuffer, int& outSize) = 0;

    /**
     * Synchronously loads a file. Provided for call sites that cannot
     * easily be converted to task chains (e.g. SceneManager scene transitions).
     *
     * CDRom backend: uses blocking readSectorsBlocking via GPU spin-loop.
     * PCdrv backend: same as the sync pcdrv_open/read/close flow.
     *
     * Returns loaded data (caller-owned), or nullptr on failure.
     */
    virtual uint8_t* LoadFileSync(const char* filename, int& outSize) = 0;

    /**
     * Optional progress-reporting variant of LoadFileSync.
     *
     * @param progress  If non-null, the backend may call progress->fn()
     *                  periodically during the load with interpolated
     *                  percentage values between startPercent and endPercent.
     *
     * Default implementation delegates to LoadFileSync and calls the
     * callback once at endPercent.  CDRom backend overrides this to
     * read in 64 KB chunks and report after each chunk.
     */
    struct LoadProgressInfo {
        void (*fn)(uint8_t percent, void* userData);
        void* userData;
        uint8_t startPercent;
        uint8_t endPercent;
    };

    virtual uint8_t* LoadFileSyncWithProgress(
        const char* filename, int& outSize,
        const LoadProgressInfo* progress)
    {
        auto* data = LoadFileSync(filename, outSize);
        if (progress && progress->fn)
            progress->fn(progress->endPercent, progress->userData);
        return data;
    }

    /** Free a buffer returned by scheduleLoadFile or LoadFileSync. */
    virtual void FreeFile(uint8_t* data) = 0;

    /** Human-readable name for logging ("pcdrv" / "cdrom"). */
    virtual const char* Name() const = 0;

    // ── Filename helpers ──────────────────────────────────────────
    // Build the correct filename for the active backend.

    /** scene_N.splashpack  or  SCENE_N.SPK;1 */
    static void BuildSceneFilename(int sceneIndex, char* out, int maxLen);

    /** scene_N.loading  or  SCENE_N.LDG;1 */
    static void BuildLoadingFilename(int sceneIndex, char* out, int maxLen);

    // ── Singleton ─────────────────────────────────────────────────
    static FileLoader& Get();
};

}  // namespace psxsplash
