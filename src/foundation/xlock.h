/*
 * xlock.h — Cross-process singleflight lock for indexing the same project.
 *
 * Issue #22: cbm_pipeline_try_lock/lock/unlock (pipeline.c) is an in-process
 * atomic_int. It cannot see another OS process, so N independently-launched
 * codebase-memory-mcp instances can each acquire their own process-local lock
 * and all start a full index of the SAME repo at once (CPU 300% -> 600%+).
 *
 * cbm_xlock_t adds a second, OS-level lock keyed by the project's db path
 * (one lock file per project under <cache_dir>/locks/). It uses POSIX
 * flock(2) advisory locking (Windows: LockFileEx), which gives us the one
 * property a hand-rolled heartbeat/lease protocol cannot give for free: the
 * kernel releases the lock automatically the instant the holder's process
 * exits for ANY reason (clean exit, crash, SIGKILL) — so a dead holder can
 * never wedge the lock (no lease timers, no liveness polling needed for
 * correctness).
 *
 * State machine (documented in issue #22, 7 states):
 *   IDLE -> ACQUIRING -> OWNER_INDEXING -> RELEASED -> IDLE
 *                    \-> WAITING -> (owner finishes)  -> RELEASED (reuse)
 *                                 -> (owner crashes)   -> CRASH_RELEASE -> ACQUIRING
 *   STALE_RECLAIM is folded into CRASH_RELEASE: because flock is used in
 *   blocking mode while WAITING, the kernel unblocks the waiter the instant
 *   the crashed holder's fd is closed by process exit — there is no separate
 *   pidfile-liveness poll to go stale.
 */
#ifndef CBM_FOUNDATION_XLOCK_H
#define CBM_FOUNDATION_XLOCK_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CBM_XLOCK_STATE_IDLE = 0,      /* not yet attempted */
    CBM_XLOCK_STATE_ACQUIRING,     /* trying non-blocking flock */
    CBM_XLOCK_STATE_OWNER,         /* acquired: this process now owns the index run */
    CBM_XLOCK_STATE_WAITING,       /* another process holds it; blocked in flock(LOCK_EX) */
    CBM_XLOCK_STATE_REUSED,        /* woke from WAITING and found a fresh completion marker */
    CBM_XLOCK_STATE_CRASH_RECLAIMED, /* woke from WAITING because the holder died; now owner */
    CBM_XLOCK_STATE_RELEASED       /* run finished (or reuse decided); lock released */
} cbm_xlock_state_t;

typedef struct {
    char lock_path[1024];
    char marker_path[1024];
    int fd;                 /* -1 when not held */
    cbm_xlock_state_t state;
    long long acquire_started_ns; /* monotonic timestamp of the acquire attempt start */
} cbm_xlock_t;

/* Derive the lock + completion-marker file paths for a project name under the
 * resolved cache dir (<cache_dir>/locks/<project>.lock / .done). */
void cbm_xlock_paths_for_project(const char *project_name, char *lock_out, size_t lock_sz,
                                 char *marker_out, size_t marker_sz);

/* Non-blocking attempt. Returns true and sets state=OWNER on success (caller
 * must index, then call cbm_xlock_mark_done + cbm_xlock_release). Returns
 * false and sets state=WAITING when another live process holds it. */
bool cbm_xlock_try_acquire(cbm_xlock_t *lk, const char *project_name);

/* Blocking wait for the current holder to release (whether by clean finish or
 * by dying — the kernel releases the flock either way). After it returns,
 * inspect cbm_xlock_marker_fresh_since() to decide REUSED vs re-acquire as
 * owner (CRASH_RECLAIMED / normal ACQUIRING retry). Returns false only on a
 * hard OS error (lock file could not be opened at all). */
bool cbm_xlock_wait_for_release(cbm_xlock_t *lk, const char *project_name);

/* True when the completion marker's timestamp is >= the acquire attempt's
 * start time, i.e. some other process finished indexing this exact project
 * AFTER we asked — safe to reuse its result instead of re-indexing. */
bool cbm_xlock_marker_fresh(const cbm_xlock_t *lk);

/* Record that indexing completed successfully (updates the marker mtime). */
void cbm_xlock_mark_done(cbm_xlock_t *lk);

/* Release the flock and close the fd. Idempotent. */
void cbm_xlock_release(cbm_xlock_t *lk);

#ifdef __cplusplus
}
#endif

#endif /* CBM_FOUNDATION_XLOCK_H */
