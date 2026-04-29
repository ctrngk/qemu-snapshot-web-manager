#ifndef DIRTY_STATE_H
#define DIRTY_STATE_H

#include "vm_backend.h"

#define DIRTY_STATE_DEFAULT_PATH "/var/lib/qswm/dirty-state.json"

int dirty_state_init(const char *path);
void dirty_state_shutdown(void);
void dirty_state_mark_clean(const char *name);
void dirty_state_mark_dirty(const char *name);
int dirty_state_is_dirty(const char *name, vm_state_t state);

#endif
