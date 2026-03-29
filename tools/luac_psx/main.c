/*
 * luac_psx - PS1 Lua bytecode compiler
 *
 * A minimal PS1 executable that reads Lua source files via PCdrv,
 * compiles them using psxlua's compiler, and writes bytecode back
 * via PCdrv. Designed to run inside PCSX-Redux headless mode as
 * a build tool.
 *
 * Links only psyqo (allocator) and psxlua (full parser). No game
 * code, no renderer, no GPU.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "pcdrv.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Internal headers for luaU_dump (supports strip parameter) */
#include "lobject.h"
#include "lstate.h"
#include "lundump.h"

/* psyqo's xprintf provides sprintf/printf via PS1 syscalls */
#include <psyqo/xprintf.h>

/* psyqo allocator - provides psyqo_realloc and psyqo_free */
#include <psyqo/alloc.h>

/*
 * Lua runtime support functions.
 *
 * psxlua declares these as extern in llibc.h when LUA_TARGET_PSX
 * is defined. Normally they're provided via linker --defsym redirects
 * to psyqo functions, but we define them directly here.
 */
int luaI_sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsprintf(str, format, ap);
    va_end(ap);
    return ret;
}

void luaI_free(void *ptr) {
    psyqo_free(ptr);
}

void *luaI_realloc(void *ptr, size_t size) {
    return psyqo_realloc(ptr, size);
}

/* Maximum source file size: 256KB should be plenty */
#define MAX_SOURCE_SIZE (256 * 1024)

/* Maximum bytecode output size */
#define MAX_OUTPUT_SIZE (256 * 1024)

/* Maximum manifest line length */
#define MAX_LINE_LEN 256

/* Bytecode writer state */
typedef struct {
    uint8_t *buf;
    size_t size;
    size_t capacity;
} WriterState;

/* lua_dump writer callback - accumulates bytecode into a buffer */
static int bytecode_writer(lua_State *L, const void *p, size_t sz, void *ud) {
    WriterState *ws = (WriterState *)ud;
    if (ws->size + sz > ws->capacity) {
        printf("ERROR: bytecode output exceeds buffer capacity\n");
        return 1;
    }
    /* memcpy via byte loop - no libc on PS1 */
    const uint8_t *src = (const uint8_t *)p;
    uint8_t *dst = ws->buf + ws->size;
    for (size_t i = 0; i < sz; i++) {
        dst[i] = src[i];
    }
    ws->size += sz;
    return 0;
}

/* Read an entire file via PCdrv into a buffer. Returns bytes read, or -1 on error. */
static int read_file(const char *path, char *buf, int max_size) {
    int fd = PCopen(path, 0, 0);  /* O_RDONLY = 0 */
    if (fd < 0) {
        printf("ERROR: cannot open '%s'\n", path);
        return -1;
    }

    int total = 0;
    while (total < max_size) {
        int chunk = max_size - total;
        if (chunk > 2048) chunk = 2048;  /* read in 2KB chunks */
        int n = PCread(fd, buf + total, chunk);
        if (n <= 0) break;
        total += n;
    }

    PCclose(fd);
    return total;
}

/* Write a buffer to a file via PCdrv. Returns 0 on success, -1 on error. */
static int write_file(const char *path, const void *buf, int size) {
    int fd = PCcreat(path, 0);
    if (fd < 0) {
        printf("ERROR: cannot create '%s'\n", path);
        return -1;
    }

    const uint8_t *p = (const uint8_t *)buf;
    int remaining = size;
    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > 2048) chunk = 2048;  /* write in 2KB chunks */
        int n = PCwrite(fd, p, chunk);
        if (n < 0) {
            printf("ERROR: write failed for '%s'\n", path);
            PCclose(fd);
            return -1;
        }
        p += n;
        remaining -= n;
    }

    PCclose(fd);
    return 0;
}

/* Simple strlen - no libc on PS1 */
static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Error log buffer - accumulates error messages for the sentinel file */
static char error_log[4096];
static int error_log_len = 0;

static void error_log_append(const char *msg) {
    int len = str_len(msg);
    if (error_log_len + len + 1 < (int)sizeof(error_log)) {
        const char *src = msg;
        char *dst = error_log + error_log_len;
        while (*src) *dst++ = *src++;
        *dst++ = '\n';
        *dst = '\0';
        error_log_len += len + 1;
    }
}

/* Write the sentinel file to signal completion */
static void write_sentinel(const char *status) {
    /* For errors, write the error log as sentinel content */
    if (str_len(status) == 5 && status[0] == 'E') {
        /* "ERROR" - write error details */
        if (error_log_len > 0)
            write_file("__done__", error_log, error_log_len);
        else
            write_file("__done__", status, str_len(status));
    } else {
        write_file("__done__", status, str_len(status));
    }
}

/* Parse the next line from the manifest buffer. Returns line length, or -1 at end. */
static int next_line(const char *buf, int buf_len, int *pos, char *line, int max_line) {
    if (*pos >= buf_len) return -1;

    int i = 0;
    while (*pos < buf_len && i < max_line - 1) {
        char c = buf[*pos];
        (*pos)++;
        if (c == '\n') break;
        if (c == '\r') continue;  /* skip CR */
        line[i++] = c;
    }
    line[i] = '\0';
    return i;
}

/* Compile a single Lua source file to bytecode */
static int compile_file(const char *input_path, const char *output_path,
                        char *source_buf, uint8_t *output_buf) {
    printf("  %s -> %s\n", input_path, output_path);

    /* Read source */
    int source_len = read_file(input_path, source_buf, MAX_SOURCE_SIZE);
    if (source_len < 0) {
        error_log_append("ERROR: cannot open source file");
        error_log_append(input_path);
        return -1;
    }

    /* Create a fresh Lua state for each file to avoid accumulating memory */
    lua_State *L = luaL_newstate();
    if (!L) {
        printf("ERROR: cannot create Lua state (out of memory)\n");
        error_log_append("ERROR: cannot create Lua state (out of memory)");
        return -1;
    }

    /* Compile source to bytecode */
    int status = luaL_loadbuffer(L, source_buf, source_len, input_path);
    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (err) {
            printf("ERROR: %s\n", err);
            error_log_append(err);
        } else {
            printf("ERROR: compilation failed for '%s'\n", input_path);
            error_log_append("ERROR: compilation failed");
            error_log_append(input_path);
        }
        lua_close(L);
        return -1;
    }

    /* Dump bytecode (strip debug info to save space) */
    WriterState ws;
    ws.buf = output_buf;
    ws.size = 0;
    ws.capacity = MAX_OUTPUT_SIZE;

    /* Use luaU_dump directly with strip=1 to remove debug info (line numbers,
     * local variable names, source filename). Saves significant space. */
    status = luaU_dump(L, getproto(L->top - 1), bytecode_writer, &ws, 1);
    lua_close(L);

    if (status != 0) {
        printf("ERROR: bytecode dump failed for '%s'\n", input_path);
        return -1;
    }

    /* Write bytecode to output file */
    if (write_file(output_path, ws.buf, ws.size) != 0) {
        return -1;
    }

    printf("  OK (%d bytes source -> %d bytes bytecode)\n", source_len, (int)ws.size);
    return 0;
}

int main(void) {
    /* Initialize PCdrv */
    PCinit();

    printf("luac_psx: PS1 Lua bytecode compiler\n");

    /* Allocate work buffers */
    char *source_buf = (char *)psyqo_realloc(NULL, MAX_SOURCE_SIZE);
    uint8_t *output_buf = (uint8_t *)psyqo_realloc(NULL, MAX_OUTPUT_SIZE);
    char *manifest_buf = (char *)psyqo_realloc(NULL, MAX_SOURCE_SIZE);

    if (!source_buf || !output_buf || !manifest_buf) {
        printf("ERROR: cannot allocate work buffers\n");
        write_sentinel("ERROR");
        while (1) {}
    }

    /* Read manifest file */
    int manifest_len = read_file("manifest.txt", manifest_buf, MAX_SOURCE_SIZE);
    if (manifest_len < 0) {
        printf("ERROR: cannot read manifest.txt\n");
        write_sentinel("ERROR");
        while (1) {}
    }

    /* Process manifest: pairs of lines (input, output) */
    int pos = 0;
    int file_count = 0;
    int error_count = 0;
    char input_path[MAX_LINE_LEN];
    char output_path[MAX_LINE_LEN];

    while (1) {
        /* Read input path */
        int len = next_line(manifest_buf, manifest_len, &pos, input_path, MAX_LINE_LEN);
        if (len < 0) break;
        if (len == 0) continue;  /* skip blank lines */

        /* Read output path */
        len = next_line(manifest_buf, manifest_len, &pos, output_path, MAX_LINE_LEN);
        if (len <= 0) {
            printf("ERROR: manifest has unpaired entry for '%s'\n", input_path);
            error_count++;
            break;
        }

        /* Compile */
        if (compile_file(input_path, output_path, source_buf, output_buf) != 0) {
            error_count++;
            break;  /* stop on first error */
        }
        file_count++;
    }

    /* Clean up */
    psyqo_free(source_buf);
    psyqo_free(output_buf);
    psyqo_free(manifest_buf);

    /* Write sentinel */
    if (error_count > 0) {
        printf("FAILED: %d file(s) compiled, %d error(s)\n", file_count, error_count);
        write_sentinel("ERROR");
    } else {
        printf("SUCCESS: %d file(s) compiled\n", file_count);
        write_sentinel("OK");
    }

    /* Halt - PCSX-Redux will kill us */
    while (1) {}
    return 0;
}
