#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <microhttpd.h>

#include "server.h"
#include "routes.h"
#include "util.h"

static struct MHD_Daemon *daemon_handle;
static const char        *g_static_dir;
static const char        *g_libvirt_uri;

/* ------------------------------------------------------------------ */
/*  helpers                                                           */
/* ------------------------------------------------------------------ */

static const char *guess_mime(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";

    if (str_eq(dot, ".html")) return "text/html";
    if (str_eq(dot, ".css"))  return "text/css";
    if (str_eq(dot, ".js"))   return "application/javascript";
    if (str_eq(dot, ".json")) return "application/json";
    if (str_eq(dot, ".png"))  return "image/png";
    if (str_eq(dot, ".svg"))  return "image/svg+xml";
    if (str_eq(dot, ".ico"))  return "image/x-icon";

    return "application/octet-stream";
}

static enum MHD_Result send_response(struct MHD_Connection *conn,
                                     unsigned int status,
                                     const char *content_type,
                                     const char *body)
{
    size_t len = strlen(body);
    char *buf = malloc(len);
    if (!buf)
        return MHD_NO;
    memcpy(buf, body, len);

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) {
        free(buf);
        return MHD_NO;
    }

    MHD_add_response_header(resp, "Content-Type", content_type);
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  static-file serving                                               */
/* ------------------------------------------------------------------ */

static enum MHD_Result serve_static_file(struct MHD_Connection *conn,
                                         const char *url)
{
    /* Path traversal protection */
    if (strstr(url, "..")) {
        return send_response(conn, MHD_HTTP_FORBIDDEN,
                             "text/plain", "Forbidden");
    }

    const char *rel = url;
    if (str_eq(url, "/"))
        rel = "/index.html";

    char *filepath = str_fmt("%s%s", g_static_dir, rel);
    if (!filepath)
        return MHD_NO;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        log_msg(LOG_DEBUG, "Static 404: %s", filepath);
        free(filepath);
        return send_response(conn, MHD_HTTP_NOT_FOUND,
                             "text/html",
                             "<html><body><h1>404 Not Found</h1></body></html>");
    }

    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(fp);
        free(filepath);
        return send_response(conn, MHD_HTTP_NOT_FOUND,
                             "text/html",
                             "<html><body><h1>404 Not Found</h1></body></html>");
    }

    size_t fsize = (size_t)st.st_size;
    char *data = malloc(fsize);
    if (!data) {
        fclose(fp);
        free(filepath);
        return MHD_NO;
    }

    size_t nread = fread(data, 1, fsize, fp);
    fclose(fp);

    if (nread != fsize) {
        free(data);
        free(filepath);
        return MHD_NO;
    }

    const char *mime = guess_mime(filepath);

    struct MHD_Response *resp =
        MHD_create_response_from_buffer(fsize, data, MHD_RESPMEM_MUST_FREE);
    if (!resp) {
        free(data);
        free(filepath);
        return MHD_NO;
    }

    MHD_add_response_header(resp, "Content-Type", mime);
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    free(filepath);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  main request handler (MHD callback)                               */
/* ------------------------------------------------------------------ */

static enum MHD_Result
request_handler(void *cls,
                struct MHD_Connection *connection,
                const char *url,
                const char *method,
                const char *version,
                const char *upload_data,
                size_t *upload_data_size,
                void **con_cls)
{
    (void)cls;
    (void)version;

    log_msg(LOG_DEBUG, "%s %s", method, url);

    /* API routes */
    if (str_starts_with(url, "/api/")) {
        if (str_eq(method, "GET") && str_eq(url, "/api/ping")) {
            return send_response(connection, MHD_HTTP_OK,
                                 "text/plain", "pong");
        }
        return route_dispatch(connection, url, method,
                              upload_data, upload_data_size, con_cls);
    }

    /* First call for non-API requests — just mark and return */
    if (*con_cls == NULL) {
        *con_cls = (void *)1;
        return MHD_YES;
    }

    /* Static files (GET only) */
    if (!str_eq(method, "GET")) {
        return send_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED,
                             "text/plain", "Method Not Allowed");
    }

    return serve_static_file(connection, url);
}

/* ------------------------------------------------------------------ */
/*  public API                                                        */
/* ------------------------------------------------------------------ */

int server_start(int port, const char *static_dir, const char *libvirt_uri)
{
    g_static_dir  = static_dir;
    g_libvirt_uri = libvirt_uri;

    routes_init();

    daemon_handle = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION,
        (uint16_t)port,
        NULL, NULL,             /* accept policy callback */
        &request_handler, NULL, /* access handler + extra arg */
        MHD_OPTION_NOTIFY_COMPLETED, &route_request_completed, NULL,
        MHD_OPTION_END);

    if (!daemon_handle) {
        log_msg(LOG_ERROR, "MHD_start_daemon failed on port %d", port);
        return -1;
    }

    log_msg(LOG_INFO, "HTTP server listening on port %d", port);
    return 0;
}

void server_stop(void)
{
    if (daemon_handle) {
        MHD_stop_daemon(daemon_handle);
        daemon_handle = NULL;
        log_msg(LOG_INFO, "HTTP server stopped");
    }
}
