#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

struct StorageInfo {
  const char *backend;
  bool mounted;
  uint64_t total_bytes;
  uint64_t used_bytes;
  uint64_t free_bytes;
};

struct FileMeta {
  bool exists;
  bool is_dir;
  uint64_t size;
  // Optional; 0 if unavailable
  time_t mtime;
};

// Abstract storage backend. Protocol and client stay backend-agnostic.
class StorageBackend {
 public:
  virtual ~StorageBackend() {}

  virtual bool begin() = 0;
  virtual void end() = 0;
  virtual const char *name() const = 0;
  virtual bool isMounted() const = 0;
  virtual StorageInfo info() const = 0;

  virtual bool exists(const char *path) = 0;
  virtual bool isDirectory(const char *path) = 0;
  virtual bool mkdir(const char *path) = 0;
  virtual bool remove(const char *path) = 0;
  virtual bool rename(const char *from, const char *to) = 0;

  virtual bool stat(const char *path, FileMeta &meta) = 0;

  // listCb returns false to stop iteration.
  virtual bool list(const char *path,
                    bool (*listCb)(const char *name, bool isDir, uint64_t size, void *ctx),
                    void *ctx) = 0;

  virtual File openRead(const char *path) = 0;
  virtual File openWrite(const char *path, bool truncate) = 0;

  // Destructive format. Returns false and fills errMsg when unsupported/failed.
  // fsType: "fat32" (supported on SD). "ntfs" is not supported by ESP32 FatFs.
  virtual bool format(const char *fsType, char *errMsg, size_t errLen) {
    if (errMsg && errLen) {
      snprintf(errMsg, errLen, "format not supported on this backend");
    }
    return false;
  }

  virtual bool supportsFormat() const { return false; }
};
