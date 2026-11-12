/*
 * host_paths.c — executable-relative and current-directory path policy.
 *
 * Kept separate from launcher.c because config, saves, mods, and command-line
 * paths all need this policy even when a game supplies its own ROM launcher.
 */
#include "host_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define snesrecomp_chdir _chdir
#  define snesrecomp_getcwd _getcwd
#  define snesrecomp_mkdir _mkdir
#else
#  include <unistd.h>
#  define snesrecomp_chdir chdir
#  define snesrecomp_getcwd getcwd
#  define snesrecomp_mkdir(path) mkdir(path, 0755)
#endif
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

/* Full path of the running executable. Returns 1 on success. Platforms
 * without a query mechanism (e.g. Switch homebrew) return 0 and callers
 * fall back to cwd-relative behavior. */
static int get_exe_path(char *out, size_t max_len) {
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)max_len);
    return (n > 0 && n < max_len) ? 1 : 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)max_len;
    return _NSGetExecutablePath(out, &size) == 0 ? 1 : 0;
#elif defined(__linux__)
    /* Inside an AppImage, /proc/self/exe resolves inside the read-only mount.
     * $APPIMAGE instead identifies the user-visible file beside config.ini,
     * rom.cfg, and saves. */
    const char *appimg = getenv("APPIMAGE");
    if (appimg && appimg[0]) {
        size_t len = strlen(appimg);
        if (len >= max_len) return 0;
        memcpy(out, appimg, len + 1);
        return 1;
    }
    ssize_t n = readlink("/proc/self/exe", out, max_len - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
#else
    (void)out;
    (void)max_len;
    return 0;
#endif
}

/* Directory containing the executable, with trailing separator.
 * Falls back to "./" when the executable path cannot be determined. */
static void get_exe_dir(char *out, size_t max_len) {
    char exe_path[1024];
    if (get_exe_path(exe_path, sizeof(exe_path))) {
        char *last_sep = NULL;
        for (char *p = exe_path; *p; p++)
            if (*p == '/' || *p == '\\') last_sep = p;
        if (last_sep) {
            *(last_sep + 1) = '\0';
            snprintf(out, max_len, "%s", exe_path);
            return;
        }
    }
    snprintf(out, max_len, "./");
}

int snesrecomp_abspath(const char *path, char *out, size_t max_len) {
    if (!path || !*path || !out || max_len == 0) return 0;
#ifdef _WIN32
    char tmp[1024];
    if (!_fullpath(tmp, path, sizeof(tmp))) return 0;
    if (strlen(tmp) >= max_len) return 0;
    strcpy(out, tmp);
    return 1;
#else
    if (path[0] == '/') {
        if (strlen(path) >= max_len) return 0;
        strcpy(out, path);
        return 1;
    }
    char cwd[1024];
    if (!snesrecomp_getcwd(cwd, sizeof(cwd))) return 0;
    if (snprintf(out, max_len, "%s/%s", cwd, path) >= (int)max_len) return 0;
    return 1;
#endif
}

int snesrecomp_exe_path(char *out, size_t max_len) {
    if (!out || max_len == 0) return 0;
    out[0] = '\0';
    return get_exe_path(out, max_len);
}

int snesrecomp_exe_basename(char *out, size_t max_len) {
    char exe_path[1024];
    if (!out || max_len == 0) return 0;
    out[0] = '\0';
    if (!get_exe_path(exe_path, sizeof(exe_path))) return 0;

    const char *base = exe_path;
    for (const char *p = exe_path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base) return 0;

    size_t len = strlen(base);
    /* Strip a trailing ".exe" so the same name serves the CMake target on
     * every platform. Case-insensitive: Windows does not promise the case. */
    if (len > 4) {
        const char *ext = base + len - 4;
        if ((ext[0] == '.') &&
            (ext[1] == 'e' || ext[1] == 'E') &&
            (ext[2] == 'x' || ext[2] == 'X') &&
            (ext[3] == 'e' || ext[3] == 'E'))
            len -= 4;
    }
    if (len >= max_len) return 0;
    memcpy(out, base, len);
    out[len] = '\0';
    return 1;
}

int snesrecomp_exe_dir_path(const char *leaf, char *out, size_t max_len) {
    if (!leaf || !out || max_len == 0) return 0;
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' &&
        (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0'))
        return 0;
    if (snprintf(out, max_len, "%s%s", dir, leaf) >= (int)max_len) return 0;
    return 1;
}

/* Probe by creating a file: access(W_OK) is unreliable on Windows and inside
 * sandboxed mounts. */
static int dir_is_writable(const char *dir) {
    char probe[1024];
    if (snprintf(probe, sizeof(probe), "%s.snesrecomp_write_probe",
                 dir) >= (int)sizeof(probe))
        return 0;
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

int snesrecomp_anchor_to_exe_dir(void) {
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' &&
        (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0'))
        return 0;
    if (!dir_is_writable(dir)) {
        fprintf(stderr,
                "[Launcher] Executable directory '%s' is not writable; "
                "config and saves stay in the current directory.\n", dir);
        return 0;
    }
    if (snesrecomp_chdir(dir) != 0) {
        fprintf(stderr, "[Launcher] Could not change directory to '%s'.\n", dir);
        return 0;
    }
    fprintf(stderr, "[Launcher] Config/saves anchored to '%s'.\n", dir);
    return 1;
}

/* SNESRECOMP_ARTIFACT_DIR is cached so an artifact path cannot change during
 * a run if the process environment is modified after startup. */
static int s_artifact_dir_cached;
static int s_artifact_dir_from_env;
static int s_artifact_dir_valid = 1;
static char s_artifact_dir_cache[1024];

static int is_path_separator(char c) {
    return c == '/' || c == '\\';
}

static void cache_artifact_dir(void) {
    if (s_artifact_dir_cached)
        return;
    s_artifact_dir_cached = 1;

    const char *configured = getenv("SNESRECOMP_ARTIFACT_DIR");
    if (!configured || !configured[0])
        return;

    s_artifact_dir_from_env = 1;
    if (!snesrecomp_abspath(configured, s_artifact_dir_cache,
                            sizeof(s_artifact_dir_cache))) {
        s_artifact_dir_valid = 0;
        return;
    }

    size_t len = strlen(s_artifact_dir_cache);
    while (len > 1 && is_path_separator(s_artifact_dir_cache[len - 1])) {
#ifdef _WIN32
        if (len == 3 && s_artifact_dir_cache[1] == ':')
            break;
#endif
        s_artifact_dir_cache[--len] = '\0';
    }
}

static int make_dir_component(const char *path) {
    if (snesrecomp_mkdir(path) == 0)
        return 1;
    return errno == EEXIST;
}

/* Create every directory component in a mutable path, ignoring EEXIST. */
static int make_dirs(char *path) {
    if (!path || !path[0])
        return 0;

    char *component = path;
#ifdef _WIN32
    if (path[0] && path[1] == ':') {
        component = path + 2;
        if (is_path_separator(*component))
            component++;
    } else if (is_path_separator(path[0]) &&
               is_path_separator(path[1])) {
        component = path + 2; /* UNC server/share paths. */
    } else if (is_path_separator(path[0])) {
        component = path + 1;
    }
#else
    if (is_path_separator(path[0]))
        component = path + 1;
#endif

    for (char *p = component; *p; p++) {
        if (!is_path_separator(*p) || p == path ||
            is_path_separator(p[-1]))
            continue;
        char separator = *p;
        *p = '\0';
        int ok = make_dir_component(path);
        *p = separator;
        if (!ok)
            return 0;
    }

    size_t len = strlen(path);
    return is_path_separator(path[len - 1]) || make_dir_component(path);
}

static int ensure_parent_dirs(const char *path) {
    size_t len = strlen(path);
    char *parent = (char *)malloc(len + 1);
    if (!parent)
        return 0;
    memcpy(parent, path, len + 1);

    char *last_separator = NULL;
    for (char *p = parent; *p; p++)
        if (is_path_separator(*p))
            last_separator = p;
    if (!last_separator) {
        free(parent);
        return 1;
    }

    if (last_separator == parent) {
        last_separator[1] = '\0';
#ifdef _WIN32
    } else if (last_separator == parent + 2 && parent[1] == ':') {
        last_separator[1] = '\0';
#endif
    } else {
        *last_separator = '\0';
    }

    int ok = make_dirs(parent);
    free(parent);
    return ok;
}

int snesrecomp_artifact_dir(char *out, size_t max_len) {
    if (!out || max_len == 0)
        return 0;
    out[0] = '\0';

    cache_artifact_dir();
    if (!s_artifact_dir_valid)
        return 0;

    if (s_artifact_dir_from_env) {
        size_t len = strlen(s_artifact_dir_cache);
        if (len >= max_len)
            return 0;
        memcpy(out, s_artifact_dir_cache, len + 1);
    } else if (!snesrecomp_exe_dir_path("", out, max_len)) {
        return 0;
    }

    if (!make_dirs(out)) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

int snesrecomp_artifact_path(const char *leaf, char *out, size_t max_len) {
    if (!leaf || !out || max_len == 0)
        return 0;
    out[0] = '\0';

    cache_artifact_dir();
    if (!s_artifact_dir_valid)
        return 0;

    if (!s_artifact_dir_from_env) {
        if (!snesrecomp_exe_dir_path(leaf, out, max_len)) {
            (void)ensure_parent_dirs(leaf);
            return 0;
        }
        if (!ensure_parent_dirs(out)) {
            out[0] = '\0';
            return 0;
        }
        return 1;
    }

    char dir[1024];
    if (!snesrecomp_artifact_dir(dir, sizeof(dir)))
        return 0;

    size_t dir_len = strlen(dir);
#ifdef _WIN32
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    int written = is_path_separator(dir[dir_len - 1])
                      ? snprintf(out, max_len, "%s%s", dir, leaf)
                      : snprintf(out, max_len, "%s%c%s", dir, separator, leaf);
    if (written < 0 || (size_t)written >= max_len) {
        out[0] = '\0';
        return 0;
    }
    if (!ensure_parent_dirs(out)) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

#ifdef SNESRECOMP_ARTIFACT_PATH_TESTING
void snesrecomp_artifact_path_reset(void) {
    s_artifact_dir_cached = 0;
    s_artifact_dir_from_env = 0;
    s_artifact_dir_valid = 1;
    s_artifact_dir_cache[0] = '\0';
}
#endif
