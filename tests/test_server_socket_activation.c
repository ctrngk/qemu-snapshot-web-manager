#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/server.h"

static int make_listening_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    int one = 1;
    assert(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 16) == 0);
    return fd;
}

static void test_server_stop_preserves_socket_activation_fd(void)
{
    int fd = make_listening_socket();

    assert(server_start_fd(fd, ".", "qemu:///system") == 0);
    server_stop();

    assert(fcntl(fd, F_GETFD) != -1);
    close(fd);

    printf("  PASS: test_server_stop_preserves_socket_activation_fd\n");
}

int main(void)
{
    printf("Running socket activation server tests...\n");
    test_server_stop_preserves_socket_activation_fd();
    printf("All %d tests passed!\n", 1);
    return 0;
}
