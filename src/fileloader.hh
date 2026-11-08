#pragma once

#include <stdint.h>
#include <psyqo/task.hh>

namespace psxsplash {

/**
 * FileLoader - abstract interface for loading files on PS1.
 *
 * The one backend is FileLoaderArchive (fileloader_archive.hh): every file
 * comes from one psyqo-paths archive, read from the CD drive, or over PCdrv
 * from the host when there is no disc. Code that needs the drive itself or
 * raw sectors includes that header; everything else needs only this one.
 *
 * The tasks follow the psyqo TaskQueue pattern (see
 * nugget/psyqo/examples/task-demo). The backend singleton is accessed through
 * FileLoader::Get().
 */
class FileLoader {
  public:
    virtual ~FileLoader() = default;

    /** Called once from Application::prepare() before any GPU work. */
    virtual void prepare() {}

    /** Returns a Task that initialises the loader: finds the archive, on the disc or over PCdrv. */
    virtual psyqo::TaskQueue::Task scheduleInit() = 0;

    /**
     * Returns a Task that loads a file.
     *
     * On resolve, *outBuffer points to the loaded data (caller owns it)
     * and *outSize contains the size in bytes.
     * On reject, *outBuffer == nullptr and *outSize == 0.
     *
     * Filenames are archive keys like "scene_0.splashpack"; use the
     * BuildSceneFilename / BuildLoadingFilename helpers.
     */
    virtual psyqo::TaskQueue::Task scheduleLoadFile(
        const char* filename, uint8_t*& outBuffer, int& outSize) = 0;

    /**
     * Synchronously loads a file, with blocking reads that spin on GPU
     * callbacks. For call sites that cannot easily be converted to task
     * chains (e.g. SceneManager scene transitions).
     *
     * Returns loaded data (caller-owned), or nullptr on failure.
     */
    virtual uint8_t* LoadFileSync(const char* filename, int& outSize) = 0;

    /** LoadFileSync for a file that is often not there (a scene's loading screen). */
    virtual uint8_t* LoadFileSyncIfThere(const char* filename, int& outSize) { return LoadFileSync(filename, outSize); }

    /**
     * Optional progress-reporting variant of LoadFileSync.
     *
     * @param progress  If non-null, the backend may call progress->fn()
     *                  periodically during the load with interpolated
     *                  percentage values between startPercent and endPercent.
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

    /** Free a buffer returned by scheduleLoadFile, LoadFileSync or loadHostFile. */
    virtual void FreeFile(uint8_t* data) = 0;

    /** Human-readable name for logging. */
    virtual const char* Name() const = 0;

    /**
     * True when the archive is read over PCdrv (an emulator's -pcdrv, or a
     * monitor serving the breaks over serial). The editor's tools then talk to
     * the engine through host files beside the image: reload.flag (hot
     * reload), luabp.txt (Lua breakpoints) and perfmap.pos (profiler walk).
     * Known from how the archive was found, not by probing for a driver.
     */
    virtual bool overPCdrv() const = 0;

    /**
     * A file beside the image on the host, read over PCdrv; nullptr when it
     * is not there or the archive is not read over PCdrv. Free with FreeFile.
     */
    uint8_t* loadHostFile(const char* filename, int& outSize);

    // -- Filename helpers ------------------------------------------

    /** scene_N.splashpack */
    static void BuildSceneFilename(int sceneIndex, char* out, int maxLen);

    /** scene_N.vram */
    static void BuildVramFilename(int sceneIndex, char* out, int maxLen);

    /** scene_N.spu */
    static void BuildSpuFilename(int sceneIndex, char* out, int maxLen);

    /** scene_N.loading */
    static void BuildLoadingFilename(int sceneIndex, char* out, int maxLen);

    /** scene_N.geo - streamed world geometry, if any */
    static void BuildGeoFilename(int sceneIndex, char* out, int maxLen);

    // -- Singleton -------------------------------------------------
    static FileLoader& Get();
};

}  // namespace psxsplash
