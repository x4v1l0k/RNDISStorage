#include "storage.h"
#include "board_config.h"
#include "path_util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
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

/** Build /sdcard + normalized path; if latin1, encode each UTF-8 component to CP1252 bytes. */
static bool vfs_path_encoded(const char *virtual_path, bool as_latin1, char *out, size_t out_len)
{
    char norm[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(virtual_path && virtual_path[0] ? virtual_path : "/", norm, sizeof(norm))) {
        return false;
    }
    if (strcmp(norm, "/") == 0) {
        snprintf(out, out_len, "%s", SD_MOUNT_POINT);
        return true;
    }
    if (!as_latin1) {
        snprintf(out, out_len, "%s%s", SD_MOUNT_POINT, norm);
        return true;
    }

    char converted[RNDISSTORAGE_MAX_PATH + 1];
    converted[0] = '/';
    converted[1] = '\0';
    size_t c_len = 1;

    char tmp[RNDISSTORAGE_MAX_PATH + 1];
    strncpy(tmp, norm, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok != NULL; tok = strtok_r(NULL, "/", &save)) {
        char l1[RNDISSTORAGE_MAX_PATH + 1];
        if (!utf8_to_fs_latin1(tok, l1, sizeof(l1))) {
            return false;
        }
        size_t t_len = strlen(l1);
        if (c_len + 1 + t_len >= sizeof(converted)) {
            return false;
        }
        if (c_len > 1) {
            converted[c_len++] = '/';
        }
        memcpy(converted + c_len, l1, t_len);
        c_len += t_len;
        converted[c_len] = '\0';
    }
    snprintf(out, out_len, "%s%s", SD_MOUNT_POINT, converted);
    return true;
}

bool storage_vfs_path(const char *virtual_path, char *out, size_t out_len)
{
    return vfs_path_encoded(virtual_path, false, out, out_len);
}

/**
 * FatFs is compiled with UTF-8 API — never pass raw Latin-1 bytes (0xED/0xF1):
 * invalid UTF-8 paths can hang inside stat/fopen.
 *
 * Resolve order:
 *  1) UTF-8 virtual path as-is
 *  2) CP437-mojibake UTF-8 (María → Marφa) for corrupted LFNs
 *  3) Parent dir scan: match fs_name_to_utf8(d_name) to basename
 * for_create: if nothing matches, keep UTF-8 path for new files.
 */
static bool resolve_vfs(const char *virtual_path, char *out, size_t out_len, bool for_create)
{
    if (!vfs_path_encoded(virtual_path, false, out, out_len)) {
        return false;
    }
    struct stat st;
    if (stat(out, &st) == 0) {
        return true;
    }

    char alt[RNDISSTORAGE_MAX_PATH + 32];
    char moj[RNDISSTORAGE_MAX_PATH + 1];
    char moj_norm[RNDISSTORAGE_MAX_PATH + 1];
    if (utf8_to_cp437_mojibake(virtual_path, moj, sizeof(moj)) &&
        normalize_path(moj, moj_norm, sizeof(moj_norm)) &&
        vfs_path_encoded(moj_norm, false, alt, sizeof(alt)) && stat(alt, &st) == 0) {
        strncpy(out, alt, out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }

    /* Directory scan: display name → actual d_name on disk. */
    char norm[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(virtual_path && virtual_path[0] ? virtual_path : "/", norm, sizeof(norm)) ||
        strcmp(norm, "/") == 0) {
        return for_create;
    }
    const char *slash = strrchr(norm, '/');
    const char *base = (slash && slash[1]) ? slash + 1 : NULL;
    if (!base) {
        return for_create;
    }
    char parent[RNDISSTORAGE_MAX_PATH + 1];
    if (slash == norm) {
        strncpy(parent, "/", sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
    } else {
        size_t plen = (size_t)(slash - norm);
        if (plen >= sizeof(parent)) {
            return for_create;
        }
        memcpy(parent, norm, plen);
        parent[plen] = '\0';
    }

    char parent_vfs[RNDISSTORAGE_MAX_PATH + 32];
    /* Parent only via UTF-8 / mojibake — never Latin-1 bytes. */
    if (!vfs_path_encoded(parent, false, parent_vfs, sizeof(parent_vfs))) {
        return for_create;
    }
    if (stat(parent_vfs, &st) != 0) {
        if (!(utf8_to_cp437_mojibake(parent, moj, sizeof(moj)) &&
              normalize_path(moj, moj_norm, sizeof(moj_norm)) &&
              vfs_path_encoded(moj_norm, false, parent_vfs, sizeof(parent_vfs)) &&
              stat(parent_vfs, &st) == 0)) {
            return for_create;
        }
    }

    DIR *dir = opendir(parent_vfs);
    if (!dir) {
        return for_create;
    }
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char utf8_name[RNDISSTORAGE_MAX_PATH + 1];
        if (!fs_name_to_utf8(ent->d_name, utf8_name, sizeof(utf8_name))) {
            continue;
        }
        if (strcmp(utf8_name, base) != 0 && strcmp(ent->d_name, base) != 0) {
            continue;
        }
        size_t pv = strlen(parent_vfs);
        int n;
        if (pv > 0 && parent_vfs[pv - 1] == '/') {
            n = snprintf(alt, sizeof(alt), "%s%s", parent_vfs, ent->d_name);
        } else {
            n = snprintf(alt, sizeof(alt), "%s/%s", parent_vfs, ent->d_name);
        }
        if (n < 0 || (size_t)n >= sizeof(alt) || (size_t)n >= out_len) {
            continue;
        }
        if (stat(alt, &st) != 0) {
            continue;
        }
        memcpy(out, alt, (size_t)n + 1);
        found = true;
        break;
    }
    closedir(dir);
    if (found) {
        return true;
    }
    return for_create;
}

bool storage_begin(void)
{
    if (s_mounted) {
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 32 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

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
        ESP_LOGW(TAG, "SD HS 4-bit failed (%s), retry default clock", esp_err_to_name(ret));
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    }
    if (ret != ESP_OK) {
        slot.width = 1;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        s_mounted = false;
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s (%d-bit, %d kHz)", SD_MOUNT_POINT, slot.width, host.max_freq_khz);
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
    if (!resolve_vfs(path, vfs, sizeof(vfs), false)) {
        return false;
    }
    struct stat st;
    return stat(vfs, &st) == 0;
}

bool storage_is_directory(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!resolve_vfs(path, vfs, sizeof(vfs), false)) {
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
    if (!resolve_vfs(path, vfs, sizeof(vfs), true)) {
        return false;
    }
    return mkdir(vfs, 0775) == 0 || errno == EEXIST;
}

bool storage_remove(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!resolve_vfs(path, vfs, sizeof(vfs), false)) {
        return false;
    }
    struct stat st;
    if (stat(vfs, &st) != 0) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        return rmdir(vfs) == 0;
    }
    return unlink(vfs) == 0;
}

typedef struct {
    char parent[RNDISSTORAGE_MAX_PATH + 1];
    bool ok;
} storage_rm_ctx_t;

static bool storage_rm_cb(const char *name, bool is_dir, uint64_t size, void *v)
{
    (void)size;
    storage_rm_ctx_t *c = (storage_rm_ctx_t *)v;
    char child[RNDISSTORAGE_MAX_PATH + 64];
    if (strcmp(c->parent, "/") == 0) {
        snprintf(child, sizeof(child), "/%s", name);
    } else {
        snprintf(child, sizeof(child), "%s/%s", c->parent, name);
    }
    child[sizeof(child) - 1] = '\0';
    if (is_dir) {
        if (!storage_remove_recursive(child)) {
            c->ok = false;
        }
    } else if (!storage_remove(child)) {
        c->ok = false;
    }
    return true;
}

bool storage_remove_recursive(const char *path)
{
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) {
        return false;
    }
    if (!storage_is_directory(path)) {
        return storage_remove(path);
    }
    storage_rm_ctx_t ctx;
    strncpy(ctx.parent, path, sizeof(ctx.parent) - 1);
    ctx.parent[sizeof(ctx.parent) - 1] = '\0';
    ctx.ok = true;
    if (!storage_list(path, storage_rm_cb, &ctx) || !ctx.ok) {
        return false;
    }
    return storage_remove(path);
}

bool storage_rename(const char *from, const char *to)
{
    char a[RNDISSTORAGE_MAX_PATH + 32];
    char b[RNDISSTORAGE_MAX_PATH + 32];
    if (!resolve_vfs(from, a, sizeof(a), false)) {
        return false;
    }
    /* Destination: prefer UTF-8 create path. */
    if (!resolve_vfs(to, b, sizeof(b), true)) {
        return false;
    }
    return rename(a, b) == 0;
}

bool storage_stat(const char *path, file_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!resolve_vfs(path, vfs, sizeof(vfs), false)) {
        meta->exists = false;
        return true;
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
    if (!resolve_vfs(path, vfs, sizeof(vfs), false)) {
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

        char disk_full[768];
        size_t vlen = strlen(vfs);
        int npath;
        if (vlen > 0 && vfs[vlen - 1] == '/') {
            npath = snprintf(disk_full, sizeof(disk_full), "%s%s", vfs, ent->d_name);
        } else {
            npath = snprintf(disk_full, sizeof(disk_full), "%s/%s", vfs, ent->d_name);
        }
        if (npath < 0 || (size_t)npath >= sizeof(disk_full)) {
            continue;
        }

        struct stat st;
        bool is_dir = false;
        uint64_t size = 0;
        if (stat(disk_full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = is_dir ? 0 : (uint64_t)st.st_size;
        }

        /* Skip Windows metadata folders. */
        if (strcmp(ent->d_name, "System Volume Information") == 0 ||
            strcmp(ent->d_name, "$RECYCLE.BIN") == 0 || strcmp(ent->d_name, "RECYCLER") == 0) {
            continue;
        }

        char utf8_name[RNDISSTORAGE_MAX_PATH + 1];
        if (!fs_name_to_utf8(ent->d_name, utf8_name, sizeof(utf8_name))) {
            continue;
        }
        if (!cb(utf8_name, is_dir, size, ctx)) {
            break;
        }
    }
    closedir(dir);
    return true;
}

FILE *storage_open_read(const char *path)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    if (!resolve_vfs(path, vfs, sizeof(vfs), false)) {
        return NULL;
    }
    return fopen(vfs, "rb");
}

FILE *storage_open_write(const char *path, bool truncate)
{
    char vfs[RNDISSTORAGE_MAX_PATH + 32];
    /* Existing file → resolve (utf8 or latin1). New file → UTF-8 path. */
    if (!resolve_vfs(path, vfs, sizeof(vfs), true)) {
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
