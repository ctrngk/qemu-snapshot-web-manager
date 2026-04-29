#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include "util.h"
#include "config.h"
#include "libvirt_backend.h"
#include "vm_backend.h"
#include "dirty_state.h"

#include "server.h"

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n"
           "\n"
           "Options:\n"
           "  -p, --port PORT        Listen port (default: 9091)\n"
           "  -s, --static-dir DIR   Static file directory (default: ./static)\n"
           "  -u, --uri URI          Libvirt connection URI (default: qemu:///system)\n"
           "      --dirty-state PATH  Dirty-state JSON path (default: %s)\n"
           "  -h, --help             Show this help message\n",
           prog, DIRTY_STATE_DEFAULT_PATH);
}

int main(int argc, char *argv[])
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    config_load_dropins(&cfg, "/etc/qswm/conf.d");

    int port = cfg.port;
    const char *static_dir = cfg.static_dir;
    const char *libvirt_uri = cfg.uri;

    static struct option long_opts[] = {
        { "port",       required_argument, NULL, 'p' },
        { "static-dir", required_argument, NULL, 's' },
        { "uri",        required_argument, NULL, 'u' },
        { "dirty-state", required_argument, NULL, 1000 },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:s:u:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return 1;
            }
            break;
        case 's':
            static_dir = optarg;
            break;
        case 'u':
            libvirt_uri = optarg;
            break;
        case 1000:
            snprintf(cfg.dirty_state_path, sizeof(cfg.dirty_state_path),
                     "%s", optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Set up signal handlers for graceful shutdown */
    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE — writing to a closed socket should not kill the server */
    signal(SIGPIPE, SIG_IGN);

    log_msg(LOG_INFO, "qemu-snapshot-web-manager starting");
    log_msg(LOG_INFO, "  port:       %d", port);
    log_msg(LOG_INFO, "  static-dir: %s", static_dir);
    log_msg(LOG_INFO, "  uri:        %s", libvirt_uri);
    log_msg(LOG_INFO, "  dirty-state: %s", cfg.dirty_state_path);

    /* Initialize libvirt backend */
    vm_backend_t *be = libvirt_backend_get();
    backend_set(be);
    if (be->connect(libvirt_uri) != 0) {
        log_msg(LOG_ERROR, "Failed to connect to libvirt");
        return 1;
    }

    if (dirty_state_init(cfg.dirty_state_path) != 0) {
        log_msg(LOG_WARN, "Dirty state persistence unavailable; continuing with empty state");
    }

    /* Try socket activation first */
    int act_fd = server_check_activation();
    int rc;
    if (act_fd >= 0) {
        log_msg(LOG_INFO, "Socket activation detected (fd %d)", act_fd);
        rc = server_start_fd(act_fd, static_dir, libvirt_uri);
    } else {
        rc = server_start(port, static_dir, libvirt_uri);
    }

    if (rc != 0) {
        log_msg(LOG_ERROR, "Failed to start server");
        be->disconnect();
        return 1;
    }

    log_msg(LOG_INFO, "Server running — press Ctrl+C to stop");

    while (running)
        sleep(1);

    log_msg(LOG_INFO, "Shutting down...");
    server_stop();
    dirty_state_shutdown();

    vm_backend_t *be2 = backend_get();
    if (be2) be2->disconnect();

    log_msg(LOG_INFO, "Bye.");

    return 0;
}
