#pragma once

#include "storage_backend.h"

// SDMMC TF-card backend for T-Dongle-S3 (primary storage for this PoC).

class SDBackend : public StorageBackend {
 public:
  bool begin() override;
  void end() override;
  const char *name() const override { return "SD-FAT"; }
  bool isMounted() const override { return mounted_; }
  StorageInfo info() const override;

  bool exists(const char *path) override;
  bool isDirectory(const char *path) override;
  bool mkdir(const char *path) override;
  bool remove(const char *path) override;
  bool rename(const char *from, const char *to) override;
  bool stat(const char *path, FileMeta &meta) override;
  bool list(const char *path,
            bool (*listCb)(const char *name, bool isDir, uint64_t size, void *ctx),
            void *ctx) override;
  File openRead(const char *path) override;
  File openWrite(const char *path, bool truncate) override;

  bool format(const char *fsType, char *errMsg, size_t errLen) override;
  bool supportsFormat() const override { return true; }

 private:
  bool mountInternal(bool oneBitMode);
  bool mounted_ = false;
};
