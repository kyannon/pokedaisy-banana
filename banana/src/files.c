// See files.h. The backups as the 3DS port keeps them (3ds/ctr/main.c).
#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return false;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.part", to);
    FILE* out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    static char buf[64 * 1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    // the copy only takes the name once it is whole
    if (ok && rename(tmp, to) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

void make_dirs(const char* path) {
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char* s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0777);
            *s = '/';
        }
    }
    mkdir(p, 0777);
}

void rom_base(const char* rom, char* out, size_t len) {
    const char* slash = strrchr(rom, '/');
    snprintf(out, len, "%s", slash ? slash + 1 : rom);
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*) a, *(const char* const*) b);
}

// Whether name is one of base's backups: exactly "<base>-YYYYmmdd-HHMMSS.sav"
// (a prefix match would count "<base>-2"'s backups as base's and prune them).
static bool is_backup_of(const char* name, const char* base) {
    size_t baseLen = strlen(base);
    if (strncmp(name, base, baseLen)) return false;
    const char* rest = name + baseLen;
    static const char PATTERN[] = "-########-######.sav";
    if (strlen(rest) != sizeof(PATTERN) - 1) return false;
    for (size_t i = 0; PATTERN[i]; i++) {
        if (PATTERN[i] == '#' ? (rest[i] < '0' || rest[i] > '9') : rest[i] != PATTERN[i]) return false;
    }
    return true;
}

static bool same_bytes(const char* a, const char* b) {
    FILE* fa = fopen(a, "rb");
    FILE* fb = fopen(b, "rb");
    bool same = fa && fb;
    static char ba[16 * 1024], bb[16 * 1024];
    while (same) {
        size_t na = fread(ba, 1, sizeof(ba), fa), nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na)) same = false;
        if (na == 0) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

void backup_save(const char* dir, const char* base, const char* savePath) {
    if (!file_exists(savePath)) return;
    make_dirs(dir);
    // the newest backup already holds these bytes: none again (ten state loads in a row would otherwise push the
    // only copy of an older save out)
    {
        char* newest = NULL;
        DIR* d = opendir(dir);
        struct dirent* e;
        while (d && (e = readdir(d))) {
            if (is_backup_of(e->d_name, base) && (!newest || strcmp(e->d_name, newest) > 0)) {
                free(newest);
                newest = strdup(e->d_name);
            }
        }
        if (d) closedir(d);
        if (newest) {
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", dir, newest);
            free(newest);
            if (same_bytes(p, savePath)) return;
        }
    }
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    char to[1024];
    snprintf(to, sizeof(to), "%s/%s-%s.sav", dir, base, stamp);
    for (int tries = 0; tries < 60 && access(to, F_OK) == 0; tries++) {
        // (a backup this second already, of other bytes: the next second's name, so none is written over)
        now++;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        snprintf(to, sizeof(to), "%s/%s-%s.sav", dir, base, stamp);
    }
    if (!copy_file(savePath, to)) {
        fprintf(stderr, "pokedaisy: couldn't back up %s to %s (%s)\n", savePath, to, strerror(errno));
        return; // never prune when the newest copy didn't land
    }

    // prune: the names sort by time, so the oldest come first
    char* names[256];
    int n = 0;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && n < 256) {
        if (is_backup_of(e->d_name, base)) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t) n, sizeof(char*), cmp_str);
    for (int i = 0; i < n; i++) {
        if (i < n - MAX_BACKUPS) {
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
            remove(p);
        }
        free(names[i]);
    }
}

void sync_file(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    fsync(fd);
    close(fd);
}
