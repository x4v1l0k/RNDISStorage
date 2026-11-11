#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool normalize_path(const char *input, char *out, size_t out_len);
bool is_safe_file_name_component(const char *component);

/** True if `s` is valid UTF-8 (ASCII-only counts as valid). */
bool is_valid_utf8(const char *s);

/**
 * Normalize an on-disk filename to UTF-8 for the Web UI.
 * If already valid UTF-8, copy as-is; otherwise treat bytes as Windows-1252/Latin-1
 * (common when FatFs was in ANSI/OEM mode) and convert to UTF-8.
 * Returns false if output buffer is too small.
 */
bool fs_name_to_utf8(const char *fs_name, char *out, size_t out_len);

/**
 * Convert a UTF-8 path/name to Latin-1/CP1252 bytes for fallback open/stat
 * of files created/listed under ANSI FatFs. Non-Latin-1 codepoints become '?'.
 */
bool utf8_to_fs_latin1(const char *utf8, char *out, size_t out_len);

/** UTF-8 Spanish letters → CP437 mojibake UTF-8 (í→φ) for fallback open. */
bool utf8_to_cp437_mojibake(const char *utf8, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
