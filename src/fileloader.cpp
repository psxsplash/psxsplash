#include "fileloader.hh"

#include <psyqo/xprintf.h>

#include "common/kernel/pcdrv.h"
#include "fileloader_archive.hh"

namespace psxsplash {

volatile const char g_discReadSpeed[] = "psxsplash disc read speed 2";

// -- Singleton ----------------------------------------------------
FileLoader& FileLoader::Get() {
    static FileLoaderArchive instance;
    return instance;
}

uint8_t* FileLoader::loadHostFile(const char* filename, int& outSize) {
    outSize = 0;
    // PCinit has run: the archive was opened over PCdrv.
    if (!overPCdrv()) return nullptr;
    int fd = PCopen(filename, 0, 0);
    if (fd < 0) return nullptr;
    int size = PClseek(fd, 0, PCDRV_SEEK_END);
    uint8_t* data = nullptr;
    if (size >= 0 && PClseek(fd, 0, PCDRV_SEEK_SET) == 0) {
        data = new uint8_t[size > 0 ? size : 1];
        if (data && size > 0 && PCread(fd, data, size) != size) {
            delete[] data;
            data = nullptr;
        }
    }
    PCclose(fd);
    if (data) outSize = size;
    return data;
}

// -- Filename helpers ---------------------------------------------
// The archive keys are the names SplashEdit gives the exported files, and
// tools/mkdisc.py stores each under its own name.
void FileLoader::BuildSceneFilename(int sceneIndex, char* out, int maxLen) {
    snprintf(out, maxLen, "scene_%d.splashpack", sceneIndex);
}

void FileLoader::BuildVramFilename(int sceneIndex, char* out, int maxLen) {
    snprintf(out, maxLen, "scene_%d.vram", sceneIndex);
}

void FileLoader::BuildSpuFilename(int sceneIndex, char* out, int maxLen) {
    snprintf(out, maxLen, "scene_%d.spu", sceneIndex);
}

void FileLoader::BuildLoadingFilename(int sceneIndex, char* out, int maxLen) {
    snprintf(out, maxLen, "scene_%d.loading", sceneIndex);
}

void FileLoader::BuildGeoFilename(int sceneIndex, char* out, int maxLen) {
    snprintf(out, maxLen, "scene_%d.geo", sceneIndex);
}

}  // namespace psxsplash
