/*
 * xlock.c — see xlock.h.
 */
#include "xlock.h"

#include "compat_fs.h" /* cbm_mkdir_p */
#include "log.h"
#include "platform.h" /* cbm_resolve_cache_dir */
#include "sha256.h"   /* cbm_sha256_hex — safe filename for an arbitrary-path lock key */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h> /* flock */
#include <sys/time.h> /* utimes */
#include <unistd.h>
#include <fcntl.h>
#endif

static long long now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Lock keys are arbitrary strings (project names OR full filesystem paths —
 * callers may pass either), so hash to a fixed-length safe filename instead
 * of using the key verbatim: a raw path contains '/' and can exceed common
 * filename-length limits. The hash also keeps distinct keys from ever
 * colliding into the same lock file (a canonicalized path already implies
 * uniqueness per project; hashing preserves that 1:1 mapping deterministically
 * across processes without needing to persist a lookup table). */
void cbm_xlock_paths_for_project(const char *project_name, char *lock_out, size_t lock_sz,
                                 char *marker_out, size_t marker_sz) {
    const char *cdir = cbm_resolve_cache_dir();
    char dir[900];
    if (cdir && cdir[0]) {
        snprintf(dir, sizeof(dir), "%s/locks", cdir);
    } else {
        snprintf(dir, sizeof(dir), "locks");
    }
    cbm_mkdir_p(dir, 0755);

    const char *key = (project_name && project_name[0]) ? project_name : "default";
    char hash_hex[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(key, strlen(key), hash_hex);
    snprintf(lock_out, lock_sz, "%s/%s.lock", dir, hash_hex);
    snprintf(marker_out, marker_sz, "%s/%s.done", dir, hash_hex);
}

/* mtime of marker_path in nanoseconds since epoch, or -1 if it does not exist. */
static long long marker_mtime_ns(const char *marker_path) {
    struct stat st;
    if (stat(marker_path, &st) != 0) {
        return -1;
    }
#if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (long long)st.st_mtime * 1000000000LL;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

#ifndef _WIN32
static int open_lock_fd(const char *lock_path) {
    return open(lock_path, O_CREAT | O_RDWR, 0644);
}
#endif

bool cbm_xlock_try_acquire(cbm_xlock_t *lk, const char *project_name) {
    memset(lk, 0, sizeof(*lk));
    lk->fd = -1;
    lk->state = CBM_XLOCK_STATE_ACQUIRING;
    lk->acquire_started_ns = now_ns();
    cbm_xlock_paths_for_project(project_name, lk->lock_path, sizeof(lk->lock_path), lk->marker_path,
                                sizeof(lk->marker_path));

#ifdef _WIN32
    HANDLE h = CreateFileA(lk->lock_path, GENERIC_READ | GENERIC_WRITE, 0 /* no sharing = exclusive */,
                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        lk->state = CBM_XLOCK_STATE_WAITING;
        return false;
    }
    lk->fd = (int)(intptr_t)h;
    lk->state = CBM_XLOCK_STATE_OWNER;
    return true;
#else
    int fd = open_lock_fd(lk->lock_path);
    if (fd < 0) {
        cbm_log_warn("xlock.open_failed", "path", lk->lock_path);
        lk->state = CBM_XLOCK_STATE_WAITING; /* fail safe: caller falls back to WAITING/in-process */
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        lk->fd = fd;
        lk->state = CBM_XLOCK_STATE_OWNER;
        return true;
    }
    close(fd);
    lk->state = CBM_XLOCK_STATE_WAITING;
    return false;
#endif
}

bool cbm_xlock_wait_for_release(cbm_xlock_t *lk, const char *project_name) {
    (void)project_name;
#ifdef _WIN32
    /* Poll-based wait on Windows: no blocking LockFileEx variant is used here to
     * keep the implementation simple; short sleep-retry converges quickly and
     * is still correct (never wedges: the OS releases the handle on holder
     * exit/crash, so CreateFileA succeeds on the very next retry). */
    for (;;) {
        HANDLE h = CreateFileA(lk->lock_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            lk->fd = (int)(intptr_t)h;
            lk->state = cbm_xlock_marker_fresh(lk) ? CBM_XLOCK_STATE_REUSED
                                                    : CBM_XLOCK_STATE_CRASH_RECLAIMED;
            return true;
        }
        Sleep(50);
    }
#else
    int fd = open_lock_fd(lk->lock_path);
    if (fd < 0) {
        cbm_log_warn("xlock.open_failed", "path", lk->lock_path);
        return false;
    }
    /* Blocking acquire: the kernel wakes us the instant the current holder
     * releases — whether by clean unlock (finished) or process exit/crash
     * (fd closed by the kernel). No polling, no timeout, no stale lease. */
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        cbm_log_warn("xlock.wait_failed", "path", lk->lock_path);
        return false;
    }
    lk->fd = fd;
    lk->state = cbm_xlock_marker_fresh(lk) ? CBM_XLOCK_STATE_REUSED : CBM_XLOCK_STATE_CRASH_RECLAIMED;
    return true;
#endif
}

bool cbm_xlock_marker_fresh(const cbm_xlock_t *lk) {
    long long mt = marker_mtime_ns(lk->marker_path);
    if (mt < 0) {
        return false; /* never completed successfully */
    }
    return mt >= lk->acquire_started_ns;
}

void cbm_xlock_mark_done(cbm_xlock_t *lk) {
    FILE *f = cbm_fopen(lk->marker_path, "wb");
    if (f) {
        fclose(f);
    }
    /* Ensure the mtime reflects "now" even if the marker already existed with
     * an older mtime (touch semantics). */
#ifndef _WIN32
    (void)utimes(lk->marker_path, NULL);
#endif
}

void cbm_xlock_release(cbm_xlock_t *lk) {
    if (lk->fd < 0) {
        return;
    }
#ifdef _WIN32
    CloseHandle((HANDLE)(intptr_t)lk->fd);
#else
    flock(lk->fd, LOCK_UN);
    close(lk->fd);
#endif
    lk->fd = -1;
    lk->state = CBM_XLOCK_STATE_RELEASED;
}
