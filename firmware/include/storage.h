#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *backend;
  bool mounted;
  uint64_t total_bytes;
  uint64_t used_bytes;
  uint64_t free_bytes;
} storage_info_t;

typedef struct {
  bool exists;
  bool is_dir;
  uint64_t size;
  time_t mtime;
} file_meta_t;

typedef bool (*storage_list_cb_t)(const char *name, bool is_dir, uint64_t size, void *ctx);

bool storage_begin(void);
void storage_end(void);
bool storage_is_mounted(void);
storage_info_t storage_info(void);

bool storage_exists(const char *path);
bool storage_is_directory(const char *path);
bool storage_mkdir(const char *path);
bool storage_remove(const char *path);
bool storage_rename(const char *from, const char *to);
bool storage_stat(const char *path, file_meta_t *meta);
bool storage_list(const char *path, storage_list_cb_t cb, void *ctx);

FILE *storage_open_read(const char *path);
FILE *storage_open_write(const char *path, bool truncate);
bool storage_touch(const char *path);

bool storage_format_fat32(char *err_msg, size_t err_len);

/** Map virtual "/" path to VFS path under SD_MOUNT_POINT. */
bool storage_vfs_path(const char *virtual_path, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
