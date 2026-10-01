#include "streamreader.hh"

#include <psyqo/kernel.hh>

#include "fileloader.hh"
#include "renderer.hh"

#if defined(LOADER_CDROM)
#include "cdromhelper.hh"
#include "fileloader_cdrom.hh"
#else
#include "pcdrv_handler.hh"
#endif

namespace psxsplash {

StreamReader& StreamReader::Get() {
    static StreamReader instance;
    return instance;
}

bool StreamReader::Open(const char* filename, StreamFile& out) {
    out = StreamFile{};
#if defined(LOADER_CDROM)
    auto& loader = static_cast<FileLoaderCDRom&>(FileLoader::Get());
    if (!loader.resolveBlocking(filename, out.lba, out.size)) return false;
#else
    int fd = pcdrv_open(filename, 0, 0);
    if (fd < 0) return false;
    int size = pcdrv_seek(fd, 0, 2);
    pcdrv_close(fd);
    if (size <= 0) return false;
    out.size = static_cast<uint32_t>(size);
#endif
    for (unsigned i = 0; i < sizeof(out.name) - 1 && filename[i]; i++) out.name[i] = filename[i];
    out.valid = true;
    return true;
}

bool StreamReader::request(const StreamFile& file, uint32_t firstSector, uint32_t sectorCount,
                           void* buffer, Callback&& callback) {
    if (!file.valid || sectorCount == 0 || m_count == kMaxQueued) return false;
    if (firstSector + sectorCount > file.sectorCount()) return false;
    if (reinterpret_cast<uintptr_t>(buffer) & 3) return false;

    Request& req = m_queue[(m_head + m_count) % kMaxQueued];
    req.file = file;
    req.firstSector = firstSector;
    req.sectorCount = sectorCount;
    req.buffer = buffer;
    req.callback = eastl::move(callback);
    m_count++;
    return true;
}

void StreamReader::update() {
    if (m_inFlight || m_count == 0) return;
    Request& req = m_queue[m_head];
    if (!startRead(req)) return;  // drive busy (a track still playing): retry next frame
    m_head = (m_head + 1) % kMaxQueued;
    m_count--;
}

bool StreamReader::startRead(Request& req) {
#if defined(LOADER_CDROM)
    auto* cdrom = static_cast<FileLoaderCDRom&>(FileLoader::Get()).getCDRomDevice();
    if (!cdrom->isIdle()) return false;

    m_inFlight = true;
    CDRomHelper::WakeDrive();
    cdrom->readSectors(req.file.lba + req.firstSector, req.sectorCount, req.buffer,
                       [this, cdrom, callback = eastl::move(req.callback)](bool success) mutable {
                           // Runs on the main loop after the action has completed, so the
                           // device is idle unless something queued behind us already
                           // started. Silence only an idle drive: CD-DA needs its IRQs.
                           if (cdrom->isIdle()) CDRomHelper::SilenceDrive();
                           m_inFlight = false;
                           if (callback) callback(success);
                       });
    return true;
#else
    bool ok = false;
    int fd = pcdrv_open(req.file.name, 0, 0);
    if (fd >= 0) {
        uint32_t offset = req.firstSector * 2048;
        uint32_t want = req.sectorCount * 2048;
        // The last sector of a file is short; pad it with zeros like a CD read.
        uint32_t avail = req.file.size > offset ? req.file.size - offset : 0;
        uint32_t len = want < avail ? want : avail;
        if (pcdrv_seek(fd, static_cast<int>(offset), 0) == static_cast<int>(offset) &&
            pcdrv_read(fd, req.buffer, static_cast<int>(len)) == static_cast<int>(len)) {
            __builtin_memset(static_cast<uint8_t*>(req.buffer) + len, 0, want - len);
            ok = true;
        }
        pcdrv_close(fd);
    }
    m_inFlight = true;
    psyqo::Kernel::queueCallback([this, ok, callback = eastl::move(req.callback)]() mutable {
        m_inFlight = false;
        if (callback) callback(ok);
    });
    return true;
#endif
}

void StreamReader::drain() {
    for (int i = 0; i < m_count; i++) m_queue[(m_head + i) % kMaxQueued].callback = nullptr;
    m_head = 0;
    m_count = 0;
    m_reserved = false;
    auto& gpu = Renderer::GetInstance().getGPU();
    while (m_inFlight) gpu.pumpCallbacks();
}

}  // namespace psxsplash
