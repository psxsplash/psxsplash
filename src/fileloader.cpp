#include "fileloader.hh"
#include <psyqo/xprintf.h>

// -- Backend selection --------------------------------------------
// LOADER_CDROM is defined by the Makefile when LOADER=cdrom.
// Default (including PCDRV_SUPPORT=1) selects the PCdrv backend.
#if defined(LOADER_CDROM)
#include "fileloader_cdrom.hh"
#else
#include "fileloader_pcdrv.hh"
#endif

namespace psxsplash {

// -- Singleton ----------------------------------------------------
FileLoader& FileLoader::Get() {
#if defined(LOADER_CDROM)
    static FileLoaderCDRom instance;
#else
    static FileLoaderPCdrv instance;
#endif
    return instance;
}

// -- Filename helpers ---------------------------------------------
// PCdrv uses lowercase names matching the files SplashControlPanel
// writes to PSXBuild/.  CDRom uses uppercase 8.3 ISO9660 names with
// the mandatory ";1" version suffix.
//
// The CDRom scene files live in a per-scene subdirectory: scene N's files
// are under "SCENE<N>/".  Two PS1 limits drive this layout:
//
//   * Boot: the BIOS boot ROM only scans the first sector of the ROOT
//     directory for SYSTEM.CNF.  A flat root with many scenes pushes
//     SYSTEM.CNF (which sorts after every "SCENE_*") into the second
//     sector, so the console never finds it and falls back to
//     cdrom:\PSX.EXE;1 (black screen).
//
//   * Runtime: psyqo's ISO9660Parser caches one directory sector at a
//     time.  A directory spanning >1 sector triggers its multi-sector
//     continuation read (sector N, then the adjacent sector N+1 issued
//     from the previous read's completion callback), which desyncs the
//     CD drive and aborts with "ReadSectorsAction got CDROM acknowledge
//     in wrong state".  One folder per scene keeps every directory
//     (root and each SCENE<N>/) inside a single sector, so that path is
//     never taken.
//
// Keep these paths in sync with the <dir name="SCENE{i}"> blocks emitted
// by SplashControlPanel.cs.
void FileLoader::BuildSceneFilename(int sceneIndex, char* out, int maxLen) {
#if defined(LOADER_CDROM)
    snprintf(out, maxLen, "SCENE%d/SCENE_%d.SPK;1", sceneIndex, sceneIndex);
#else
    snprintf(out, maxLen, "scene_%d.splashpack", sceneIndex);
#endif
}

void FileLoader::BuildVramFilename(int sceneIndex, char* out, int maxLen) {
#if defined(LOADER_CDROM)
    snprintf(out, maxLen, "SCENE%d/SCENE_%d.VRM;1", sceneIndex, sceneIndex);
#else
    snprintf(out, maxLen, "scene_%d.vram", sceneIndex);
#endif
}

void FileLoader::BuildSpuFilename(int sceneIndex, char* out, int maxLen) {
#if defined(LOADER_CDROM)
    snprintf(out, maxLen, "SCENE%d/SCENE_%d.SPU;1", sceneIndex, sceneIndex);
#else
    snprintf(out, maxLen, "scene_%d.spu", sceneIndex);
#endif
}

void FileLoader::BuildLoadingFilename(int sceneIndex, char* out, int maxLen) {
#if defined(LOADER_CDROM)
    snprintf(out, maxLen, "SCENE%d/SCENE_%d.LDG;1", sceneIndex, sceneIndex);
#else
    snprintf(out, maxLen, "scene_%d.loading", sceneIndex);
#endif
}

}  // namespace psxsplash
