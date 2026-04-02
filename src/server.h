#ifndef SERVER_H
#define SERVER_H

/* Start server by binding to the given port (normal mode) */
int  server_start(int port, const char *static_dir, const char *libvirt_uri);

/* Start server using a pre-opened socket fd (socket activation mode) */
int  server_start_fd(int fd, const char *static_dir, const char *libvirt_uri);

/* Check if systemd passed us a socket. Returns fd >= 0 or -1 if not activated. */
int  server_check_activation(void);

void server_stop(void);

/* Returns seconds since last HTTP request (0 if no request yet) */
long server_idle_seconds(void);

#endif
