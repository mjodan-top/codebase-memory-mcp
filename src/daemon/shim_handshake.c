#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "daemon/shim_handshake.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    CBM_SHIM_HS_LINE_MAX = 128,
};

static const char CBM_SHIM_HELLO_TAG[] = "CBM-SHIM-HELLO";
static const char CBM_DAEMON_HELLO_TAG[] = "CBM-DAEMON-HELLO";
static const char CBM_SHIM_HS_OK_TAG[] = "OK";
static const char CBM_SHIM_HS_MISMATCH_TAG[] = "VERSION_MISMATCH";

const char *cbm_shim_hs_result_name(cbm_shim_hs_result_t result) {
    switch (result) {
    case CBM_SHIM_HS_OK:
        return "HS_OK";
    case CBM_SHIM_HS_VERSION_MISMATCH:
        return "HS_VERSION_MISMATCH";
    case CBM_SHIM_HS_IO_ERROR:
        return "HS_IO_ERROR";
    case CBM_SHIM_HS_TIMEOUT:
        return "HS_TIMEOUT";
    case CBM_SHIM_HS_MALFORMED:
        return "HS_MALFORMED";
    }
    return "HS_UNKNOWN";
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Bounded, deadline-aware full write. Returns 0 on success, -1 on any
 * timeout/error (never partial-writes-then-gives-up silently). */
static int write_all_deadline(int fd, const char *buf, size_t len, long long deadline_at_ms) {
    size_t off = 0;
    while (off < len) {
        long long remaining = deadline_at_ms - now_ms();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr = poll(&pfd, 1, (int)(remaining > 60000 ? 60000 : remaining));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Bounded, deadline-aware read of a single '\n'-terminated line (byte at a
 * time — handshake lines are tiny, so this is not a performance path).
 * Writes at most cap-1 bytes plus NUL into out. Returns 0 on success (line
 * terminated by '\n', trimmed of trailing CR/LF), -1 on timeout/error/EOF/
 * overflow. */
static int read_line_deadline(int fd, char *out, size_t cap, long long deadline_at_ms) {
    size_t len = 0;
    for (;;) {
        long long remaining = deadline_at_ms - now_ms();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, (int)(remaining > 60000 ? 60000 : remaining));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            /* Peer closed mid-handshake (midstream loss during negotiation). */
            errno = ECONNRESET;
            return -1;
        }
        if (c == '\n') {
            while (len > 0 && out[len - 1] == '\r') {
                len--;
            }
            out[len] = '\0';
            return 0;
        }
        if (len + 1 >= cap) {
            errno = EMSGSIZE;
            return -1;
        }
        out[len++] = c;
    }
}

cbm_shim_hs_result_t cbm_shim_handshake_client(int fd, int deadline_ms) {
    if (fd < 0 || deadline_ms <= 0) {
        return CBM_SHIM_HS_IO_ERROR;
    }
    long long deadline_at = now_ms() + deadline_ms;

    char hello[CBM_SHIM_HS_LINE_MAX];
    int hlen =
        snprintf(hello, sizeof(hello), "%s %d\n", CBM_SHIM_HELLO_TAG, CBM_SHIM_PROTOCOL_VERSION);
    if (hlen < 0 || (size_t)hlen >= sizeof(hello)) {
        return CBM_SHIM_HS_IO_ERROR;
    }
    if (write_all_deadline(fd, hello, (size_t)hlen, deadline_at) != 0) {
        return errno == ETIMEDOUT ? CBM_SHIM_HS_TIMEOUT : CBM_SHIM_HS_IO_ERROR;
    }

    char reply[CBM_SHIM_HS_LINE_MAX];
    if (read_line_deadline(fd, reply, sizeof(reply), deadline_at) != 0) {
        return errno == ETIMEDOUT ? CBM_SHIM_HS_TIMEOUT : CBM_SHIM_HS_IO_ERROR;
    }

    char tag[32] = {0};
    int peer_version = -1;
    char verdict[32] = {0};
    if (sscanf(reply, "%31s %d %31s", tag, &peer_version, verdict) != 3 ||
        strcmp(tag, CBM_DAEMON_HELLO_TAG) != 0) {
        return CBM_SHIM_HS_MALFORMED;
    }
    /* Defense-in-depth: never trust the daemon's self-reported "OK" verdict
     * alone. A fake/misbehaving daemon (or a genuinely mismatched build) could
     * echo verdict=OK while advertising a different protocol version; the
     * client independently re-checks peer_version against its own compiled-in
     * CBM_SHIM_PROTOCOL_VERSION before ever accepting the connection as
     * attached. Any mismatch is fail-closed regardless of what verdict says. */
    if (peer_version != CBM_SHIM_PROTOCOL_VERSION) {
        return CBM_SHIM_HS_VERSION_MISMATCH;
    }
    if (strcmp(verdict, CBM_SHIM_HS_OK_TAG) == 0) {
        return CBM_SHIM_HS_OK;
    }
    if (strcmp(verdict, CBM_SHIM_HS_MISMATCH_TAG) == 0) {
        return CBM_SHIM_HS_VERSION_MISMATCH;
    }
    return CBM_SHIM_HS_MALFORMED;
}

cbm_shim_hs_result_t cbm_shim_handshake_server(int fd, int deadline_ms) {
    if (fd < 0 || deadline_ms <= 0) {
        return CBM_SHIM_HS_IO_ERROR;
    }
    long long deadline_at = now_ms() + deadline_ms;

    char line[CBM_SHIM_HS_LINE_MAX];
    if (read_line_deadline(fd, line, sizeof(line), deadline_at) != 0) {
        return errno == ETIMEDOUT ? CBM_SHIM_HS_TIMEOUT : CBM_SHIM_HS_IO_ERROR;
    }

    char tag[32] = {0};
    int client_version = -1;
    if (sscanf(line, "%31s %d", tag, &client_version) != 2 ||
        strcmp(tag, CBM_SHIM_HELLO_TAG) != 0) {
        return CBM_SHIM_HS_MALFORMED;
    }

    bool match = (client_version == CBM_SHIM_PROTOCOL_VERSION);
    char reply[CBM_SHIM_HS_LINE_MAX];
    int rlen =
        snprintf(reply, sizeof(reply), "%s %d %s\n", CBM_DAEMON_HELLO_TAG,
                 CBM_SHIM_PROTOCOL_VERSION, match ? CBM_SHIM_HS_OK_TAG : CBM_SHIM_HS_MISMATCH_TAG);
    if (rlen < 0 || (size_t)rlen >= sizeof(reply)) {
        return CBM_SHIM_HS_IO_ERROR;
    }
    if (write_all_deadline(fd, reply, (size_t)rlen, deadline_at) != 0) {
        return errno == ETIMEDOUT ? CBM_SHIM_HS_TIMEOUT : CBM_SHIM_HS_IO_ERROR;
    }
    return match ? CBM_SHIM_HS_OK : CBM_SHIM_HS_VERSION_MISMATCH;
}
