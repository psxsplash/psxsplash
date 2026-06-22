#pragma once

#include <stdint.h>

#include <psyqo-lua/lua.hh>
#include <psyqo/memory-card-filesystem.hh>
#include <psyqo/memory-card.hh>

namespace psxsplash {

/**
 * Game-level memory card configuration, authored in the SplashEdit panel and
 * packed into the splashpack. It supplies the Sony product code used to build
 * compliant filenames, the default save title shown in the BIOS, and the save
 * icon. Sensible defaults are used when a scene has no configuration (e.g. an
 * older splashpack), so saving still works out of the box.
 */
struct MemoryCardConfig {
    bool valid = false;
    char region[3] = {'B', 'A', '\0'};                          // e.g. "BA"/"BE"/"BI"
    char product[12] = {'S', 'L', 'U', 'S', '-', '0', '0', '0', '0', '0', '\0', '\0'};  // e.g. "SLUS-00000"
    char titlePrefix[33] = {'P', 'S', 'X', 'S', 'P', 'L', 'A', 'S', 'H', '\0'};         // ASCII, up to 32 chars
    psyqo::MemoryCardFileSystem::Icon icon{};
};

/**
 * High-level, game-facing memory card service.
 *
 * Owns the low-level psyqo `MemoryCard` device and a `MemoryCardFileSystem`,
 * holds the per-game `MemoryCardConfig`, and bridges Lua tables to Sony-format
 * save files. Accessed as a singleton, mirroring `FileLoader::Get()`.
 *
 * Every operation returns a static error string (null on success) so that the
 * Lua layer can report exactly what went wrong; nothing fails silently. Saving
 * and loading are blocking operations — they are meant to run at a deliberate
 * save point, not in the middle of gameplay.
 */
class MemoryCardManager {
  public:
    using Port = psyqo::MemoryCard::Port;

    static MemoryCardManager& Get();

    /** Initialises the SIO0 bus. Call once from Application::prepare(). */
    void prepare();

    /** Applies a configuration parsed from a splashpack. */
    void setConfig(const MemoryCardConfig& config);

    /** Maps a Lua port number (0 or 1) to a Port; returns false if invalid. */
    static bool portFromIndex(int index, Port* outPort);

    // All of the following return nullptr on success, or a static human
    // readable error string on failure.

    /** Reports whether a card is present (presence itself is not an error). */
    const char* isPresent(Port port, bool* outPresent);

    /** Writes a fresh, empty Sony filesystem to the card. */
    const char* format(Port port);

    /**
     * Serializes the Lua value at `tableIdx` and writes it as a save named by
     * `key`. If `titleOrNull` is non-null it overrides the configured title.
     */
    const char* save(Port port, const char* key, const char* titleOrNull, psyqo::Lua& lua, int tableIdx);

    /** Loads and deserializes a save, pushing the resulting value on success. */
    const char* load(Port port, const char* key, psyqo::Lua& lua);

    /** Deletes a save. */
    const char* remove(Port port, const char* key);

    /** Counts free 8KiB blocks. */
    const char* freeBlocks(Port port, uint32_t* outBlocks);

    /** Lists the files on the card. */
    const char* listFiles(Port port, psyqo::MemoryCardFileSystem::FileEntry* out, uint32_t maxEntries,
                          uint32_t* outCount);

    /** Builds the Sony filename (region + product + key) into out[21]. */
    void buildFilename(const char* key, char out[21]);

  private:
    MemoryCardManager() = default;

    psyqo::MemoryCard m_card;
    psyqo::MemoryCardFileSystem m_fs{m_card};
    MemoryCardConfig m_config;
};

}  // namespace psxsplash
