#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool normalize_path(const char *input, char *out, size_t out_len);
bool is_safe_file_name_component(const char *component);

#ifdef __cplusplus
}
#endif
