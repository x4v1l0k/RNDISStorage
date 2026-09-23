#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  UI_BOOTING = 0,
  UI_READY,
  UI_FAILURE,
  UI_TRANSFERRING,
  UI_BUSY,
} ui_status_t;

bool status_display_begin(void);
void status_display_set_status(ui_status_t status);
void status_display_set_storage(uint64_t total, uint64_t used, uint64_t free_bytes);
void status_display_set_backend(const char *backend);
void status_display_refresh(void);

#ifdef __cplusplus
}
#endif
