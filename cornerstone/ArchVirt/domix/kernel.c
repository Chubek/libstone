/* The bundled Domix system: a freestanding kernel and resident shell.
 * Files come from the packed image. Mutable files live in guest RAM; commands
 * never execute host code or make host filesystem calls. */
#include "archvirt_boot.h"
#include <stddef.h>

#define MAX_FILES 128
#define PATH_SIZE 128
#define LINE_SIZE 256
#define RAM_FILE_SIZE 4096
#define CONSOLES 4
#define CELLS 2000

struct File { char path[PATH_SIZE]; const unsigned char* data; uint64_t size; unsigned mode; int deleted, ram_slot; };
struct Console { uint16_t cells[CELLS]; unsigned cursor, length; char line[LINE_SIZE], cwd[PATH_SIZE]; };
static struct File files[MAX_FILES];
static unsigned file_count;
static unsigned char ram_files[32][RAM_FILE_SIZE];
static unsigned ram_count;
static unsigned char ram_used[32];
static struct Console consoles[CONSOLES];
static unsigned active;
static const struct ArchVirtBootInfo* boot;
static int shutdown_requested;

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = dst; const unsigned char* s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
void* memset(void* dst, int value, size_t n) {
    unsigned char* d = dst;
    for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)value;
    return dst;
}
static size_t length(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static int equal(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; } return *a == *b;
}
static int prefix(const char* a, const char* b) {
    while (*b) if (*a++ != *b++) return 0; return 1;
}
static void copy(char* dst, const char* src) {
    size_t n = length(src); if (n >= PATH_SIZE) n = PATH_SIZE - 1;
    memcpy(dst, src, n); dst[n] = 0;
}
static unsigned char input(void) {
#if defined(__x86_64__)
    unsigned char c; __asm__ volatile("inb %1, %0" : "=a"(c) : "d"((uint16_t)boot->serial_base)); return c;
#else
    return *(volatile unsigned char*)(uintptr_t)boot->serial_base;
#endif
}
static void serial(unsigned char c) {
#if defined(__x86_64__)
    __asm__ volatile("outb %0, %1" : : "a"(c), "d"((uint16_t)boot->serial_base));
#else
    *(volatile unsigned char*)(uintptr_t)boot->serial_base = c;
#endif
}
static void refresh(void) {
    volatile uint16_t* screen = (volatile uint16_t*)(uintptr_t)boot->console_base;
    for (unsigned i = 0; i < CELLS; ++i) screen[i] = consoles[active].cells[i];
}
static void clear(void) {
    for (unsigned i = 0; i < CELLS; ++i) consoles[active].cells[i] = 0x0720;
    consoles[active].cursor = 0; refresh();
}
static void put(unsigned char c) {
    struct Console* con = &consoles[active];
    serial(c);
    if (c == '\r') return;
    if (c == '\n') con->cursor += 80 - con->cursor % 80;
    else if (c == '\b') {
        if (con->cursor) {
            con->cells[--con->cursor] = 0x0720;
            ((volatile uint16_t*)(uintptr_t)boot->console_base)[con->cursor] = 0x0720;
        }
    } else if (c >= 32) {
        uint16_t cell = (uint16_t)(0x0700 | c);
        con->cells[con->cursor] = cell;
        ((volatile uint16_t*)(uintptr_t)boot->console_base)[con->cursor++] = cell;
    }
    if (con->cursor >= CELLS) {
        for (unsigned i = 0; i < CELLS - 80; ++i) con->cells[i] = con->cells[i + 80];
        for (unsigned i = CELLS - 80; i < CELLS; ++i) con->cells[i] = 0x0720;
        con->cursor = CELLS - 80;
        refresh();
    }
}
static void print(const char* s) { while (*s) put((unsigned char)*s++); }
static void number(uint64_t n) {
    char digits[24]; unsigned i = 0;
    do { digits[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (i) put((unsigned char)digits[--i]);
}
static uint64_t le(const unsigned char* p, unsigned n) {
    uint64_t result = 0; for (unsigned i = 0; i < n; ++i) result |= (uint64_t)p[i] << (i * 8); return result;
}
static int mount_image(void) {
    const unsigned char* p = (const unsigned char*)(uintptr_t)boot->image_base;
    const unsigned char* end = p + boot->image_size;
    if (boot->image_size < 40) return 0;
    const char* magic = "DOMIXIMG";
    for (unsigned i = 0; i < 8; ++i) if (p[i] != (unsigned char)magic[i]) return 0;
    if (le(p + 8, 4) != 1 || le(p + 12, 4) != boot->architecture) return 0;
    uint64_t kernel_size = le(p + 16, 8), manifest_size = le(p + 24, 8), count = le(p + 32, 8);
    p += 40;
    if (kernel_size > (uint64_t)(end - p)) return 0; p += kernel_size;
    if (manifest_size > (uint64_t)(end - p) || count > MAX_FILES) return 0; p += manifest_size;
    for (uint64_t i = 0; i < count; ++i) {
        if (end - p < 4) return 0;
        uint64_t path_size = le(p, 4); p += 4;
        if (!path_size || path_size >= PATH_SIZE - 1 || path_size > (uint64_t)(end - p)) return 0;
        struct File* f = &files[file_count++];
        f->ram_slot = -1;
        f->path[0] = '/'; memcpy(f->path + 1, p, path_size); f->path[path_size + 1] = 0; p += path_size;
        if (end - p < 12) return 0;
        f->mode = (unsigned)le(p, 4); f->size = le(p + 4, 8); p += 12;
        if (f->size > (uint64_t)(end - p)) return 0;
        f->data = p; p += f->size;
    }
    return p == end;
}
static int path(const char* arg, char* out) {
    char raw[PATH_SIZE * 2]; size_t n = 0;
    if (*arg != '/') {
        const char* cwd = consoles[active].cwd;
        while (*cwd) raw[n++] = *cwd++;
        if (n > 1) raw[n++] = '/';
    }
    if (n + length(arg) >= sizeof(raw)) return 0;
    while (*arg) raw[n++] = *arg++;
    raw[n] = 0;
    unsigned used = 1; out[0] = '/';
    for (size_t i = 0; i < n;) {
        while (raw[i] == '/') ++i;
        size_t start = i; while (raw[i] && raw[i] != '/') ++i;
        size_t len = i - start;
        if (!len || (len == 1 && raw[start] == '.')) continue;
        if (len == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            while (used > 1 && out[--used] != '/') {} continue;
        }
        if (used + len + 1 >= PATH_SIZE) return 0;
        if (used > 1) out[used++] = '/';
        memcpy(out + used, raw + start, len); used += (unsigned)len;
    }
    out[used] = 0; return 1;
}
static struct File* find(const char* name) {
    for (unsigned i = 0; i < file_count; ++i)
        if (!files[i].deleted && equal(files[i].path, name)) return &files[i];
    return 0;
}
static int directory(const char* name) {
    if (equal(name, "/")) return 1;
    struct File* f = find(name);
    if (f && (f->mode & 0040000)) return 1;
    size_t n = length(name);
    for (unsigned i = 0; i < file_count; ++i)
        if (!files[i].deleted && prefix(files[i].path, name) && files[i].path[n] == '/') return 1;
    return 0;
}
static int write_file(const char* name, const unsigned char* data, uint64_t size, unsigned mode) {
    if (size > RAM_FILE_SIZE || equal(name, "/")) return 0;
    char parent[PATH_SIZE]; copy(parent, name);
    size_t end = length(parent);
    while (end > 1 && parent[--end] != '/') {}
    parent[end == 0 ? 1 : end] = 0;
    if (!directory(parent)) return 0;
    struct File* f = find(name);
    if (f && directory(name)) return 0;
    int slot = f ? f->ram_slot : -1;
    if (slot < 0) {
        for (slot = 0; slot < 32 && ram_used[slot]; ++slot) {}
        if (slot == 32) return 0;
    }
    if (!f) {
        unsigned i = 0; while (i < file_count && !files[i].deleted) ++i;
        if (i == MAX_FILES) return 0;
        if (i == file_count) ++file_count;
        f = &files[i]; copy(f->path, name);
    }
    if (!ram_used[slot]) { ram_used[slot] = 1; ++ram_count; }
    unsigned char* bytes = ram_files[slot];
    if (size) memcpy(bytes, data, (size_t)size);
    f->data = bytes; f->size = size; f->mode = mode; f->deleted = 0; f->ram_slot = slot; return 1;
}
static void remove_file(struct File* f) {
    if (f->ram_slot >= 0) { ram_used[f->ram_slot] = 0; --ram_count; f->ram_slot = -1; }
    f->deleted = 1;
}
static void list(const char* name) {
    if (!directory(name)) { print("ls: not a directory\n"); return; }
    size_t n = length(name);
    char seen[MAX_FILES][PATH_SIZE]; unsigned seen_count = 0;
    for (unsigned i = 0; i < file_count; ++i) {
        if (files[i].deleted || !prefix(files[i].path, name)) continue;
        const char* s = files[i].path + n;
        if (n > 1) { if (*s++ != '/') continue; }
        if (!*s) continue;
        char child[PATH_SIZE]; unsigned j = 0;
        while (s[j] && s[j] != '/') { child[j] = s[j]; ++j; } child[j] = 0;
        unsigned k = 0; while (k < seen_count && !equal(seen[k], child)) ++k;
        if (k != seen_count) continue;
        copy(seen[seen_count++], child); print(child); if (s[j]) put('/'); put('\n');
    }
}
static int tokenize(char* line, char** argv) {
    int argc = 0; char* r = line; char* w = line;
    while (*r) {
        while (*r == ' ' || *r == '\t') ++r;
        if (!*r) break;
        if (argc == 16) return -1;
        argv[argc++] = w;
        char quote = 0;
        while (*r && (quote || (*r != ' ' && *r != '\t'))) {
            char ch = *r++;
            if ((ch == '\'' || ch == '"') && (!quote || quote == ch)) { quote = quote ? 0 : ch; continue; }
            if (ch == '\\' && *r && quote != '\'') ch = *r++;
            *w++ = ch;
        }
        if (quote) return -1;
        while (*r == ' ' || *r == '\t') ++r;
        *w++ = 0;
    }
    return argc;
}
static void dispatch(char* line) {
    char* argv[16]; int argc = tokenize(line, argv);
    if (argc < 0) { print("shell: invalid quoting or too many arguments\n"); return; }
    if (!argc) return;
    const char* cmd = argv[0]; char a[PATH_SIZE], b[PATH_SIZE];
    if (!path(argc > 1 ? argv[1] : ".", a) || !path(argc > 2 ? argv[2] : ".", b)) {
        print("shell: path too long\n"); return;
    }
    if (equal(cmd, "help")) print("help uname version cmdline pwd cd ls cat echo touch mkdir cp mv rm wc clear whoami id mount df exit poweroff\n");
    else if (equal(cmd, "uname")) print(boot->architecture == 243 ? "Domix riscv64\n" : boot->architecture == 183 ? "Domix aarch64\n" : "Domix x86_64\n");
    else if (equal(cmd, "version")) print("Domix 0.2 / DomIOS boot ABI 1\n");
    else if (equal(cmd, "cmdline")) {
        if (boot->cmdline_base <= boot->memory_size && boot->cmdline_size <= boot->memory_size - boot->cmdline_base)
            for (uint64_t i = 0; i < boot->cmdline_size; ++i) put(((const unsigned char*)(uintptr_t)boot->cmdline_base)[i]);
        put('\n');
    }
    else if (equal(cmd, "pwd")) { print(consoles[active].cwd); put('\n'); }
    else if (equal(cmd, "cd")) { if (directory(a)) copy(consoles[active].cwd, a); else print("cd: not a directory\n"); }
    else if (equal(cmd, "ls")) list(a);
    else if (equal(cmd, "cat") || equal(cmd, "wc")) {
        struct File* f = find(a);
        if (argc < 2 || !f || (f->mode & 0040000)) print("file not found\n");
        else if (equal(cmd, "wc")) { number(f->size); put('\n'); }
        else for (uint64_t i = 0; i < f->size; ++i) put(f->data[i]);
    } else if (equal(cmd, "echo")) {
        int redirect = -1;
        for (int i = 1; i < argc; ++i) if (equal(argv[i], ">")) { redirect = i; break; }
        if (redirect < 0) { for (int i = 1; i < argc; ++i) { if (i > 1) put(' '); print(argv[i]); } put('\n'); }
        else if (redirect + 2 != argc || !path(argv[redirect + 1], a)) print("echo: expected > file\n");
        else {
            unsigned char text[LINE_SIZE]; unsigned n = 0;
            for (int i = 1; i < redirect; ++i) { if (i > 1) text[n++] = ' '; for (const char* s = argv[i]; *s; ++s) text[n++] = (unsigned char)*s; }
            text[n++] = '\n'; if (!write_file(a, text, n, 0644)) print("echo: write failed\n");
        }
    } else if (equal(cmd, "touch") || equal(cmd, "mkdir")) {
        if (argc != 2 || (!find(a) && !write_file(a, 0, 0, equal(cmd, "mkdir") ? 0040755 : 0644))) print("create failed\n");
    } else if (equal(cmd, "cp") || equal(cmd, "mv")) {
        struct File* f = find(a);
        if (argc != 3 || !f || directory(a) || directory(b) || !write_file(b, f->data, f->size, f->mode)) print("copy failed\n");
        else if (equal(cmd, "mv") && !equal(a, b)) remove_file(f);
    } else if (equal(cmd, "rm")) {
        struct File* f = find(a); if (argc != 2 || !f || directory(a)) print("rm: file not found\n"); else remove_file(f);
    } else if (equal(cmd, "clear")) clear();
    else if (equal(cmd, "whoami")) print("root\n");
    else if (equal(cmd, "id")) print("uid=0(root) gid=0(root)\n");
    else if (equal(cmd, "mount")) print("domix-image on / (ram overlay, volatile)\n");
    else if (equal(cmd, "df")) { print("image bytes: "); number(boot->image_size); print("; RAM file slots: "); number(32 - ram_count); put('\n'); }
    else if (equal(cmd, "exit") || equal(cmd, "poweroff")) { print("Domix shutdown\n"); shutdown_requested = 1; }
    else { print(cmd); print(": command not found\n"); }
}
static void prompt(void) { print("domix# "); }

void kernel_main(const struct ArchVirtBootInfo* info) {
    boot = info;
    if (!boot || boot->magic != ARCHVIRT_BOOT_MAGIC || boot->version != 1 ||
        boot->console_columns != 80 || boot->console_rows != 25 ||
        boot->image_base > boot->memory_size || boot->image_size > boot->memory_size - boot->image_base) return;
    for (unsigned i = 0; i < CONSOLES; ++i) { active = i; clear(); copy(consoles[i].cwd, "/"); }
    active = 0;
    print("Domix 0.2 booting on "); print(boot->architecture == 243 ? "riscv64\n" : boot->architecture == 183 ? "aarch64\n" : "x86_64\n");
    if (!mount_image()) { print("Domix: invalid filesystem image\n"); return; }
    if (!find("/init")) { print("Domix: missing /init\n"); return; }
    print("Root filesystem mounted; resident shell ready\n");
    struct File* init = find("/init");
    char script_line[LINE_SIZE]; unsigned script_length = 0;
    for (uint64_t i = 0; i <= init->size; ++i) {
        unsigned char c = i == init->size ? '\n' : init->data[i];
        if (c == '\n') {
            script_line[script_length] = 0;
            if (script_length && script_line[0] != '#') dispatch(script_line);
            script_length = 0;
        } else if (script_length < LINE_SIZE - 1) script_line[script_length++] = (char)c;
    }
    if (!shutdown_requested) prompt();
    while (!shutdown_requested) {
        unsigned char c = input(); if (!c) continue;
        if (c >= 0x11 && c <= 0x14) {
            active = c - 0x11; refresh();
            if (!consoles[active].cursor) prompt();
            continue;
        }
        struct Console* con = &consoles[active];
        if (c == '\n' || c == '\r') {
            put('\n'); con->line[con->length] = 0; dispatch(con->line); con->length = 0;
            if (!shutdown_requested) prompt();
        } else if (c == 8 || c == 127) {
            if (con->length) { --con->length; put('\b'); }
        } else if (c >= 32 && c < 127 && con->length < LINE_SIZE - 1) {
            con->line[con->length++] = (char)c; put(c);
        }
    }
}
