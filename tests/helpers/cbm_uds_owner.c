#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "daemon/uds_lifecycle.h"

#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t active_listen_fd = -1;

static void request_stop(int signo) {
    (void)signo;
    stop_requested = 1;
    if (active_listen_fd >= 0)
        (void)close((int)active_listen_fd);
    active_listen_fd = -1;
}

static void print_state(cbm_uds_state_t state, void *userdata) {
    (void)userdata;
    printf("STATE %s\n", cbm_uds_state_name(state));
    fflush(stdout);
}

static int run_foreign_listener(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("foreign socket");
        return 70;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        close(fd);
        return 64;
    }
    memcpy(addr.sun_path, socket_path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    if (bind(fd, (const struct sockaddr *)&addr, addr_len) < 0 || listen(fd, 4) < 0) {
        perror("foreign bind/listen");
        close(fd);
        return 71;
    }
    active_listen_fd = fd;
    printf("FOREIGN_READY path=%s pid=%ld\n", socket_path, (long)getpid());
    fflush(stdout);
    while (!stop_requested) {
        int client = accept(fd, NULL, NULL);
        if (client >= 0)
            close(client);
        else if (!stop_requested && errno != EINTR) {
            perror("foreign accept");
            break;
        }
    }
    active_listen_fd = -1;
    close(fd);
    unlink(socket_path);
    return 0;
}

static int run_probe(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("probe socket");
        return 70;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        close(fd);
        return 64;
    }
    memcpy(addr.sun_path, socket_path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    if (connect(fd, (const struct sockaddr *)&addr, addr_len) < 0) {
        perror("probe connect");
        close(fd);
        return 71;
    }
    static const char ping[] = "PING\n";
    if (write(fd, ping, sizeof(ping) - 1) != (ssize_t)(sizeof(ping) - 1)) {
        perror("probe write");
        close(fd);
        return 72;
    }
    char response[16];
    ssize_t n = read(fd, response, sizeof(response));
    close(fd);
    if (n != 5 || memcmp(response, "PONG\n", 5) != 0) {
        fprintf(stderr, "probe rejected or invalid response\n");
        return 73;
    }
    printf("PROBE_OK path=%s\n", socket_path);
    return 0;
}

static int serve_connection(cbm_uds_connection_t *connection) {
    char request[16];
    ssize_t n = read(connection->fd, request, sizeof(request));
    if (n == 5 && memcmp(request, "PING\n", 5) == 0) {
        static const char pong[] = "PONG\n";
        return write(connection->fd, pong, sizeof(pong) - 1) == (ssize_t)(sizeof(pong) - 1) ? 0
                                                                                            : -1;
    }
    errno = EPROTO;
    return -1;
}

int main(int argc, char **argv) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0) {
        perror("sigaction");
        return 70;
    }

    if (argc == 3 && strcmp(argv[1], "--probe") == 0)
        return run_probe(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--foreign-listener") == 0)
        return run_foreign_listener(argv[2]);
    if ((argc != 3 && argc != 5) || strcmp(argv[1], "--socket") != 0 ||
        (argc == 5 && strcmp(argv[3], "--expected-uid") != 0)) {
        fprintf(stderr,
                "usage: %s --socket PATH [--expected-uid UID] | --probe PATH | --foreign-listener "
                "PATH\n",
                argv[0]);
        return 64;
    }

    unsigned long expected_uid = (unsigned long)-1;
    if (argc == 5) {
        char *end = NULL;
        errno = 0;
        expected_uid = strtoul(argv[4], &end, 10);
        if (errno || !end || *end != '\0')
            return 64;
    }

    cbm_uds_owner_t owner;
    cbm_uds_owner_configure(&owner, expected_uid, print_state, NULL);
    if (cbm_uds_owner_open(&owner, argv[2], 16) < 0) {
        int saved = errno;
        fprintf(stderr, "OWNER_REJECTED path=%s errno=%d error=%s\n", argv[2], saved,
                strerror(saved));
        return saved == EADDRINUSE ? 73 : 74;
    }

    active_listen_fd = owner.listen_fd;
    printf("READY path=%s pid=%ld\n", argv[2], (long)getpid());
    fflush(stdout);
    while (!stop_requested) {
        cbm_uds_connection_t connection;
        if (cbm_uds_owner_accept(&owner, &connection) < 0) {
            if (stop_requested)
                break;
            fprintf(stderr, "ACCEPT_REJECTED errno=%d error=%s\n", errno, strerror(errno));
            fflush(stderr);
            continue;
        }
        if (serve_connection(&connection) < 0)
            fprintf(stderr, "PROBE_IO_ERROR errno=%d error=%s\n", errno, strerror(errno));
        else {
            printf("PEER_OK uid=%lu\n", connection.peer_uid);
            fflush(stdout);
        }
        cbm_uds_connection_close(&connection);
    }
    if (stop_requested)
        owner.listen_fd = -1;
    active_listen_fd = -1;
    cbm_uds_owner_close(&owner);
    printf("STOPPED path=%s\n", argv[2]);
    fflush(stdout);
    return 0;
}
