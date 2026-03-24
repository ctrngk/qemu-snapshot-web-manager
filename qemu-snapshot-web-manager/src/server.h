#ifndef SERVER_H
#define SERVER_H

int  server_start(int port, const char *static_dir, const char *libvirt_uri);
void server_stop(void);

#endif
