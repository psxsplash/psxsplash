#include "streamreader.hh"

#include <psyqo/kernel.hh>

#include "cdromhelper.hh"
#include "fileloader_archive.hh"
#include "renderer.hh"

namespace psxsplash {

StreamReader& StreamReader::Get() {
    static StreamReader instance;
    return instance;
}

bool StreamReader::Open(const char* filename, StreamFile& out) {
    out = StreamFile{};
    auto& loader = FileLoaderArchive::Get();
    if (!loader.resolveBlocking(filename, out.lba, out.size)) return false;
    out.valid = true;
    return true;
}

bool StreamReader::ReadBlocking(const StreamFile& file, uint32_t firstSector, uint32_t sectorCount,
                                void* buffer) {
    if (!file.valid || sectorCount == 0 || firstSector + sectorCount > file.sectorCount()) return false;
    return FileLoaderArchive::Get().readSectorsBlocking(file.lba + firstSector, sectorCount, buffer);
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
    auto& loader = FileLoaderArchive::Get();
    auto* cdrom = loader.getCDRomDevice();
    if (!cdrom) {
        // The image over PCdrv: a synchronous read, completed on the main loop
        // as a drive read is, so callbacks keep their ordering.
        bool ok = loader.readSectorsBlocking(req.file.lba + req.firstSector, req.sectorCount, req.buffer);
        m_inFlight = true;
        psyqo::Kernel::queueCallback([this, ok, callback = eastl::move(req.callback)]() mutable {
            m_inFlight = false;
            if (callback) callback(ok);
        });
        return true;
    }
    if (!cdrom->isIdle()) return false;

    m_inFlight = true;
    CDRomHelper::WakeDrive();
    // A read that fails goes again at single speed (FileLoaderArchive::readSectorsAsync).
    loader.readSectorsAsync(req.file.lba + req.firstSector, req.sectorCount, req.buffer,
                            [this, cdrom, callback = eastl::move(req.callback)](bool success) mutable {
                                // Runs on the main loop after the action has completed, so the
                                // device is idle unless something queued behind us already
                                // started. Silence only an idle drive: CD-DA needs its IRQs.
                                if (cdrom->isIdle()) CDRomHelper::SilenceDrive();
                                m_inFlight = false;
                                if (callback) callback(success);
                            });
    return true;
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
