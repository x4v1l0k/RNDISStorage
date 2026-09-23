#include "storage.h"
#include "board_config.h"
#include "path_util.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

bool storage_vfs_path(const char *virtual_path, char *out, size_t out_len)
{
    char norm[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(virtual_path && virtual_path[0] ? virtual_path : "/", norm, sizeof(norm))) {
        return false;
    }
    if (strcmp(norm, "/") == 0) {
        snprintf(out, out_len, "%s", SD_MOUNT_POINT);
        return true;
    }
    snprintf(out, out_len, "%s%s", SD_MOUNT_POINT, norm);
    return true;
}

bool storage_begin(void)
{
    if (s_mounted) {
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.d1 = PIN_SD_D1;
    slot.d2 = PIN_SD_D2;
    slot.d3 = PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (ret != ESP_OK) {
        // Retry 1-bit
        slot.width = 1;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        s_mounted = false;
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    return true;
}

void storage_end(void)
{
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
    }
}

bool storage_is_mounted(void) { return s_mounted; }

storage_info_t storage_info(void)
{
    storage_info_t i = {.backend = "SD-FAT", .mounted = s_mounted};
    if (!s_mounted || !s_card) {
        return i;
    }
    uint64_t total = 0, free_b = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b);
    i.total_bytes = total;
    i.free_bytes = free_b;
    i.used_bytes = (total > free_b) ? (total - free_b) : 0;
    return i;
}

bool storage_exists(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return false;
    }
    struct stat st;
    return stat(vfs, &st) == 0;
}

bool storage_is_directory(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return false;
    }
    struct stat st;
    if (stat(vfs, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

bool storage_mkdir(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return false;
    }
    return mkdir(vfs, 0775) == 0 || errno == EEXIST;
}

bool storage_remove(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return false;
    }
    if (storage_is_directory(path)) {
        return rmdir(vfs) == 0;
    }
    return unlink(vfs) == 0;
}

bool storage_rename(const char *from, const char *to)
{
    char a[RNDISSTORAGE_MAX_PATH + 32];
    char b[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(from, a, sizeof(a)) || !storage_vfs_path(to, b, sizeof(b))) {
        return false;
    }
    return rename(a, b) == 0;
}

bool storage_stat(const char *path, file_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return false;
    }
    struct stat st;
    if (stat(vfs, &st) != 0) {
        meta->exists = false;
        return true;
    }
    meta->exists = true;
    meta->is_dir = S_ISDIR(st.st_mode);
    meta->size = meta->is_dir ? 0 : (uint64_t)st.st_size;
    meta->mtime = st.st_mtime;
    return true;
}

bool storage_list(const char *path, storage_list_cb_t cb, void *ctx)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return false;
    }
    DIR *dir = opendir(vfs);
    if (!dir) {
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child_virt[RNDISSTORAGE_MAX_PATH + 64];
        if (strcmp(path, "/") == 0) {
            snprintf(child_virt, sizeof(child_virt), "/%s", ent->d_name);
        } else {
            snprintf(child_virt, sizeof(child_virt), "%s/%s", path, ent->d_name);
        }
        child_virt[sizeof(child_virt) - 1] = '\0';
        file_meta_t meta;
        storage_stat(child_virt, &meta);
        if (!cb(ent->d_name, meta.is_dir, meta.size, ctx)) {
            break;
        }
    }
    closedir(dir);
    return true;
}

FILE *storage_open_read(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return NULL;
    }
    return fopen(vfs, "rb");
}

FILE *storage_open_write(const char *path, bool truncate)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!storage_vfs_path(path, vfs, sizeof(vfs))) {
        return NULL;
    }
    return fopen(vfs, truncate ? "wb" : "ab");
}

bool storage_touch(const char *path)
{
    FILE *f = storage_open_write(path, true);
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

bool storage_format_fat32(char *err_msg, size_t err_len)
{
    if (err_msg && err_len) {
        snprintf(err_msg, err_len, "format via USB-NCM build: use PC FAT32 or re-enable Arduino format helper");
    }
    return false;
}
