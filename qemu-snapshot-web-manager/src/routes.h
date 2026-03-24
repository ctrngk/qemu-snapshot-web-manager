#ifndef ROUTES_H
#define ROUTES_H

#include <microhttpd.h>

/* Initialize routes (called once at startup). */
void routes_init(void);

/* Main route dispatcher. Returns MHD_YES on success, MHD_NO on failure.
 * Called by server.c for all /api/ requests (except /api/ping).
 *
 * Handles POST body data via the con_cls pattern:
 * - First call (*con_cls == NULL): create post processor, return MHD_YES
 * - Subsequent calls: accumulate upload_data
 * - Final call (upload_data_size == 0): process request and send response
 */
enum MHD_Result route_dispatch(struct MHD_Connection *connection,
                                const char *url,
                                const char *method,
                                const char *upload_data,
                                size_t *upload_data_size,
                                void **con_cls);

/* Cleanup connection state (called by MHD when connection closes). */
void route_request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls,
                              enum MHD_RequestTerminationCode toe);

#endif
