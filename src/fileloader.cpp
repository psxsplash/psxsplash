#include "fileloader.hh"
#include <psyqo/xprintf.h>

// ── Backend selection ────────────────────────────────────────────
// LOADER_CDROM is defined by the Makefile when LOADER=cdrom.
// Default (including PCDRV_SUPPORT=1) selects the PCdrv backend.
#if defined(LOADER_CDROM)
#include "fileloader_cdrom.hh"
#else
#include "fileloader_pcdrv.hh"
#endif

namespace psxsplash {

// ── Singleton ────────────────────────────────────────────────────
FileLoader& FileLoader::Get() {
#if defined(LOADER_CDROM)
    static FileLoaderCDRom instance;
#else
    static FileLoaderPCdrv instance;
#endif
    return instance;
}

// ── Filename helpers ─────────────────────────────────────────────
// PCdrv uses lowercase names matching the files SplashControlPanel
// writes to PSXBuild/.  CDRom uses uppercase 8.3 ISO9660 names with
// the mandatory ";1" version suffix.

void FileLoader::BuildSceneFilename(int sceneIndex, char* out, int maxLen) {
#if defined(LOADER_CDROM)
    snprintf(out, maxLen, "SCENE_%d.SPK;1", sceneIndex);
#else
    snprintf(out, maxLen, "scene_%d.splashpack", sceneIndex);
#endif
}

void FileLoader::BuildLoadingFilename(int sceneIndex, char* out, int maxLen) {
#if defined(LOADER_CDROM)
    snprintf(out, maxLen, "SCENE_%d.LDG;1", sceneIndex);
#else
    snprintf(out, maxLen, "scene_%d.loading", sceneIndex);
#endif
}

}  // namespace psxsplash
