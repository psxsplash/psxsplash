#include "memorycardmanager.hh"

#include "luatableserializer.hh"
#include "sjis.hh"

namespace psxsplash {

// Scratch buffer for serialize/deserialize. 16 KiB comfortably covers a couple
// of card blocks of game state while staying tiny relative to the 2 MiB of RAM.
static uint8_t s_buffer[16384];

MemoryCardManager& MemoryCardManager::Get() {
    static MemoryCardManager instance;
    return instance;
}

void MemoryCardManager::prepare() { m_card.prepare(); }

void MemoryCardManager::setConfig(const MemoryCardConfig& config) { m_config = config; }

bool MemoryCardManager::portFromIndex(int index, Port* outPort) {
    if (index == 0) {
        *outPort = Port::Port0;
        return true;
    }
    if (index == 1) {
        *outPort = Port::Port1;
        return true;
    }
    return false;
}

void MemoryCardManager::buildFilename(const char* key, char out[21]) {
    int p = 0;
    for (int i = 0; i < 2 && m_config.region[i]; i++) out[p++] = m_config.region[i];
    for (int i = 0; m_config.product[i] && p < 20; i++) out[p++] = m_config.product[i];
    if (key) {
        for (int i = 0; key[i] && p < 20; i++) out[p++] = key[i];
    }
    out[p] = '\0';
}

const char* MemoryCardManager::isPresent(Port port, bool* outPresent) {
    *outPresent = false;
    psyqo::MemoryCard::Error error = m_card.probeBlocking(port);
    if (error == psyqo::MemoryCard::Error::OK) {
        *outPresent = true;
        return nullptr;
    }
    if (error == psyqo::MemoryCard::Error::NoCard) {
        // Absence is a normal state, not an error.
        return nullptr;
    }
    return psyqo::MemoryCard::errorMessage(error);
}

const char* MemoryCardManager::format(Port port) {
    psyqo::MemoryCard::Error error = m_fs.format(port);
    if (error != psyqo::MemoryCard::Error::OK) return psyqo::MemoryCard::errorMessage(error);
    return nullptr;
}

const char* MemoryCardManager::save(Port port, const char* key, const char* titleOrNull, psyqo::Lua& lua,
                                    int tableIdx) {
    uint32_t size = 0;
    const char* serError = nullptr;
    if (!LuaTableSerializer::serialize(lua, tableIdx, s_buffer, sizeof(s_buffer), &size, &serError)) {
        return serError ? serError : "failed to serialize save data";
    }

    uint8_t title[64];
    asciiToSjisTitle(titleOrNull ? titleOrNull : m_config.titlePrefix, title);

    char filename[21];
    buildFilename(key, filename);

    psyqo::MemoryCard::Error error = m_fs.writeFile(port, filename, title, m_config.icon, s_buffer, size);
    if (error != psyqo::MemoryCard::Error::OK) return psyqo::MemoryCard::errorMessage(error);
    return nullptr;
}

const char* MemoryCardManager::load(Port port, const char* key, psyqo::Lua& lua) {
    char filename[21];
    buildFilename(key, filename);

    uint32_t length = 0;
    psyqo::MemoryCard::Error error = m_fs.readFile(port, filename, s_buffer, sizeof(s_buffer), &length);
    if (error != psyqo::MemoryCard::Error::OK) return psyqo::MemoryCard::errorMessage(error);

    const char* deError = nullptr;
    if (!LuaTableSerializer::deserialize(lua, s_buffer, length, &deError)) {
        return deError ? deError : "failed to deserialize save data";
    }
    return nullptr;
}

const char* MemoryCardManager::remove(Port port, const char* key) {
    char filename[21];
    buildFilename(key, filename);
    psyqo::MemoryCard::Error error = m_fs.deleteFile(port, filename);
    if (error != psyqo::MemoryCard::Error::OK) return psyqo::MemoryCard::errorMessage(error);
    return nullptr;
}

const char* MemoryCardManager::freeBlocks(Port port, uint32_t* outBlocks) {
    psyqo::MemoryCard::Error error = m_fs.getFreeBlockCount(port, outBlocks);
    if (error != psyqo::MemoryCard::Error::OK) return psyqo::MemoryCard::errorMessage(error);
    return nullptr;
}

const char* MemoryCardManager::listFiles(Port port, psyqo::MemoryCardFileSystem::FileEntry* out, uint32_t maxEntries,
                                         uint32_t* outCount) {
    psyqo::MemoryCard::Error error = m_fs.listFiles(port, out, maxEntries, outCount);
    if (error != psyqo::MemoryCard::Error::OK) return psyqo::MemoryCard::errorMessage(error);
    return nullptr;
}

}  // namespace psxsplash
