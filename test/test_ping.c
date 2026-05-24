/* test/test_ping.c
 *
 * Automated ping/pong test for giza_server.
 *
 * Sends GSP_MSG_PING and expects GSP_MSG_PONG back.
 * Validates magic, version, and message type fields.
 *
 * Exit 0 = pass, exit 1 = fail.
 *
 * Build via:  make check
 * Or manually:
 *   gcc -o test/test_ping test/test_ping.c -Iviewer && ./test/test_ping
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "giza-server-protocol.h"

static char sock_path[256];

static void
make_sock_path(void)
{
    snprintf(sock_path, sizeof(sock_path),
             GIZA_SERVER_SOCK_DIR "/" GIZA_SERVER_SOCK_NAME,
             (int)getuid());
}

static int
connect_to_server(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int
main(void)
{
    make_sock_path();
    printf("test_ping: connecting to %s\n", sock_path);

    int fd = connect_to_server();
    if (fd < 0) {
        fprintf(stderr, "FAIL: cannot connect — is giza_server running?\n");
        return 1;
    }

    /* Send PING */
    gsp_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic   = GSP_MAGIC;
    h.version = GSP_VERSION;
    h.type    = GSP_MSG_PING;
    h.flags   = 0;
    h.length  = 0;
    h.seq     = 1;

    if (write(fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) {
        fprintf(stderr, "FAIL: write PING\n");
        close(fd); return 1;
    }

    /* Receive PONG */
    gsp_header_t r;
    memset(&r, 0, sizeof(r));
    if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) {
        fprintf(stderr, "FAIL: read PONG (short read)\n");
        close(fd); return 1;
    }

    if (r.magic != GSP_MAGIC) {
        fprintf(stderr, "FAIL: bad magic 0x%08X (expected 0x%08X)\n",
                r.magic, GSP_MAGIC);
        close(fd); return 1;
    }
    if (r.version != GSP_VERSION) {
        fprintf(stderr, "FAIL: bad version %d (expected %d)\n",
                r.version, GSP_VERSION);
        close(fd); return 1;
    }
    if (r.type != GSP_MSG_PONG) {
        fprintf(stderr, "FAIL: expected PONG (0x%02X), got 0x%02X\n",
                GSP_MSG_PONG, r.type);
        close(fd); return 1;
    }

    close(fd);
    printf("PASS: ping/pong OK\n");
    return 0;
}
