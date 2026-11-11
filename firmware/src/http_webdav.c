#include "http_webdav.h"
#include "board_config.h"
#include "path_util.h"
#include "status_display.h"
#include "storage.h"
#include "net_pivot.h"
#include "socks_proxy.h"
#include "wifi_ap.h"
#include "www_index.h"

#include "lwip/sockets.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lwip/inet.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

/** Transfer buffer on heap. Fall back to a smaller size if heap is tight. */
static char *io_chunk_alloc(size_t *out_size)
{
    const size_t try_sizes[] = {RNDISSTORAGE_IO_CHUNK, 4096, 2048};
    for (size_t i = 0; i < sizeof(try_sizes) / sizeof(try_sizes[0]); i++) {
        char *p = (char *)heap_caps_malloc(try_sizes[i], MALLOC_CAP_DEFAULT);
        if (!p) {
            p = (char *)malloc(try_sizes[i]);
        }
        if (p) {
            if (out_size) {
                *out_size = try_sizes[i];
            }
            return p;
        }
    }
    if (out_size) {
        *out_size = 0;
    }
    return NULL;
}

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** In-place percent-decode (path=%2F → /). */
static void url_decode_inplace(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = hex_nibble(r[1]);
            int lo = hex_nibble(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        if (*r == '+') {
            *w++ = ' ';
            r++;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/** Resolve path from X-Rndis-Path header (preferred) or ?path= query. */
static bool request_path(httpd_req_t *req, char *out, size_t out_len)
{
    if (out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (httpd_req_get_hdr_value_str(req, "X-Rndis-Path", out, out_len) == ESP_OK && out[0]) {
        url_decode_inplace(out);
        return true;
    }
    /* Query string may be long when path is percent-encoded. */
    char *query = (char *)malloc(1024);
    if (!query) {
        strncpy(out, "/", out_len - 1);
        out[out_len - 1] = '\0';
        return false;
    }
    if (httpd_req_get_url_query_str(req, query, 1024) != ESP_OK) {
        free(query);
        strncpy(out, "/", out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
    if (httpd_query_key_value(query, "path", out, out_len) != ESP_OK) {
        strncpy(out, "/", out_len - 1);
        out[out_len - 1] = '\0';
        free(query);
        return true;
    }
    free(query);
    url_decode_inplace(out);
    if (out[0] == '\0') {
        strncpy(out, "/", out_len - 1);
        out[out_len - 1] = '\0';
    }
    return true;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool first;
} list_buf_t;

static bool list_json_cb(const char *name, bool is_dir, uint64_t size, void *ctx)
{
    list_buf_t *b = (list_buf_t *)ctx;
    /* Escape JSON while preserving UTF-8 filename bytes (ñ, á, …). */
    char esc[RNDISSTORAGE_MAX_PATH * 2 + 16];
    size_t ei = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && ei + 6 < sizeof(esc); ++p) {
        if (*p == '"' || *p == '\\') {
            esc[ei++] = '\\';
            esc[ei++] = (char)*p;
        } else if (*p < 0x20) {
            ei += (size_t)snprintf(esc + ei, sizeof(esc) - ei, "\\u%04x", (unsigned)*p);
        } else {
            esc[ei++] = (char)*p;
        }
    }
    esc[ei] = '\0';

    char item[sizeof(esc) + 96];
    int n = snprintf(item, sizeof(item),
                     "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%llu,\"type\":\"%s\"}",
                     b->first ? "" : ",", esc, is_dir ? "true" : "false",
                     (unsigned long long)size, is_dir ? "dir" : "file");
    if (n < 0 || b->len + (size_t)n + 1 > b->cap) {
        return false;
    }
    memcpy(b->buf + b->len, item, (size_t)n);
    b->len += (size_t)n;
    b->buf[b->len] = '\0';
    b->first = false;
    return true;
}

static esp_err_t send_json(httpd_req_t *req, int code, const char *body)
{
    if (code == 200) {
        httpd_resp_set_status(req, HTTPD_200);
    } else if (code == 400) {
        httpd_resp_set_status(req, "400 Bad Request");
    } else if (code == 404) {
        httpd_resp_set_status(req, HTTPD_404);
    } else if (code == 503) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buflen)
{
    int total = req->content_len;
    if (total < 0 || (size_t)total >= buflen) {
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, WWW_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static bool probe_host_tcp(const char *ip, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (!inet_aton(ip, &dest.sin_addr)) {
        close(fd);
        return false;
    }
    bool ok = connect(fd, (struct sockaddr *)&dest, sizeof(dest)) == 0;
    close(fd);
    return ok;
}

static esp_err_t h_info(httpd_req_t *req)
{
    storage_info_t si = storage_info();
    char body[1200];
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"device\":\"%s\",\"version\":\"%s\",\"transport\":\"usb-ncm+softap\","
             "\"ip\":\"%s\",\"wifi\":{\"up\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},"
             "\"pivot\":{\"up\":%s,\"napt\":true,\"usb_dongle_ip\":\"%s\",\"usb_host_ip\":\"%s\","
             "\"socks\":{\"up\":%s,\"port\":%d},"
             "\"hint\":\"Preferred: curl --socks5 %s:%d http://%s:PORT/  (or ssh -o ProxyCommand=... )\"},"
             "\"webdav_root\":\"/dav/\","
             "\"storage\":{\"backend\":\"%s\",\"mounted\":%s,\"total_bytes\":%llu,"
             "\"used_bytes\":%llu,\"free_bytes\":%llu,\"supports_format\":false}}",
             RNDISSTORAGE_DEVICE_NAME, RNDISSTORAGE_PROTOCOL_VERSION, RNDISSTORAGE_IP_ADDR,
             wifi_ap_is_up() ? "true" : "false", wifi_ap_ssid(), wifi_ap_ip(),
             net_pivot_is_up() ? "true" : "false", RNDISSTORAGE_IP_ADDR, net_pivot_host_ip(),
             socks_proxy_is_up() ? "true" : "false", socks_proxy_port(), wifi_ap_ip(), socks_proxy_port(),
             net_pivot_host_ip(), si.backend, si.mounted ? "true" : "false",
             (unsigned long long)si.total_bytes, (unsigned long long)si.used_bytes,
             (unsigned long long)si.free_bytes);
    return send_json(req, 200, body);
}

static esp_err_t h_pivot_probe(httpd_req_t *req)
{
    char q[64];
    uint16_t port = 1234;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(q, "port", p, sizeof(p)) == ESP_OK) {
            int v = atoi(p);
            if (v > 0 && v < 65536) {
                port = (uint16_t)v;
            }
        }
    }
    bool ok = probe_host_tcp(net_pivot_host_ip(), port, 1500);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"status\":\"%s\",\"host\":\"%s\",\"port\":%u,\"reachable\":%s,"
             "\"socks\":\"%s:%d\"}",
             ok ? "ok" : "error", net_pivot_host_ip(), port, ok ? "true" : "false", wifi_ap_ip(),
             socks_proxy_port());
    return send_json(req, ok ? 200 : 503, body);
}

static esp_err_t h_list(httpd_req_t *req)
{
    if (!storage_is_mounted()) {
        return send_json(req, 503, "{\"status\":\"error\",\"error\":\"no_storage\",\"message\":\"SD not mounted\"}");
    }
    char q[RNDISSTORAGE_MAX_PATH + 1];
    request_path(req, q, sizeof(q));
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(q, path, sizeof(path))) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    char *buf = malloc(16384);
    if (!buf) {
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"oom\",\"message\":\"oom\"}");
    }
    list_buf_t lb = {.buf = buf, .len = 0, .cap = 16384, .first = true};
    lb.len = (size_t)snprintf(buf, 16384, "{\"status\":\"ok\",\"path\":\"%s\",\"entries\":[", path);
    if (!storage_list(path, list_json_cb, &lb)) {
        free(buf);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"list_failed\",\"message\":\"cannot list path\"}");
    }
    if (lb.len + 3 < lb.cap) {
        memcpy(buf + lb.len, "]}", 3);
        lb.len += 2;
    } else {
        free(buf);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"too_many\",\"message\":\"directory too large\"}");
    }
    esp_err_t err = send_json(req, 200, buf);
    free(buf);
    return err;
}

static esp_err_t path_from_json(httpd_req_t *req, char *path, size_t path_len)
{
    char body[256];
    char p[RNDISSTORAGE_MAX_PATH + 1];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!json_get_string(body, "path", p, sizeof(p)) || !normalize_path(p, path, path_len)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t h_mkdir(httpd_req_t *req)
{
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (path_from_json(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    status_display_set_status(UI_TRANSFERRING);
    bool ok = storage_mkdir(path);
    status_display_set_status(UI_READY);
    return send_json(req, ok ? 200 : 500,
                     ok ? "{\"status\":\"ok\"}"
                        : "{\"status\":\"error\",\"error\":\"mkdir_failed\",\"message\":\"mkdir failed\"}");
}

static esp_err_t h_touch(httpd_req_t *req)
{
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (path_from_json(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    bool ok = storage_touch(path);
    return send_json(req, ok ? 200 : 500,
                     ok ? "{\"status\":\"ok\"}"
                        : "{\"status\":\"error\",\"error\":\"touch_failed\",\"message\":\"touch failed\"}");
}

static esp_err_t h_delete(httpd_req_t *req)
{
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (path_from_json(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    bool ok = storage_remove_recursive(path);
    return send_json(req, ok ? 200 : 500,
                     ok ? "{\"status\":\"ok\"}"
                        : "{\"status\":\"error\",\"error\":\"delete_failed\",\"message\":\"delete failed\"}");
}

static esp_err_t h_rename(httpd_req_t *req)
{
    char body[320];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_json\",\"message\":\"body\"}");
    }
    char a[RNDISSTORAGE_MAX_PATH + 1], b[RNDISSTORAGE_MAX_PATH + 1];
    char from[RNDISSTORAGE_MAX_PATH + 1], to[RNDISSTORAGE_MAX_PATH + 1];
    if (!json_get_string(body, "path", a, sizeof(a)) || !json_get_string(body, "new_path", b, sizeof(b)) ||
        !normalize_path(a, from, sizeof(from)) || !normalize_path(b, to, sizeof(to))) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    bool ok = storage_rename(from, to);
    return send_json(req, ok ? 200 : 500,
                     ok ? "{\"status\":\"ok\"}"
                        : "{\"status\":\"error\",\"error\":\"rename_failed\",\"message\":\"rename failed\"}");
}

static bool parse_byte_range(httpd_req_t *req, uint64_t file_size, uint64_t *start, uint64_t *end)
{
    *start = 0;
    *end = file_size ? file_size - 1 : 0;
    char range[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) != ESP_OK) {
        return false;
    }
    unsigned long long s = 0, e = 0;
    if (sscanf(range, "bytes=%llu-%llu", &s, &e) == 2) {
        if (file_size == 0 || s >= file_size || e < s) {
            return false;
        }
        if (e >= file_size) {
            e = file_size - 1;
        }
        *start = s;
        *end = e;
        return true;
    }
    if (sscanf(range, "bytes=%llu-", &s) == 1) {
        if (file_size == 0 || s >= file_size) {
            return false;
        }
        *start = s;
        *end = file_size - 1;
        return true;
    }
    return false;
}

static esp_err_t stream_file(httpd_req_t *req, const char *path)
{
    file_meta_t meta;
    if (!storage_stat(path, &meta) || !meta.exists || meta.is_dir) {
        return send_json(req, 404, "{\"status\":\"error\",\"error\":\"not_found\",\"message\":\"file not found\"}");
    }
    FILE *f = storage_open_read(path);
    if (!f) {
        return send_json(req, 404, "{\"status\":\"error\",\"error\":\"not_found\",\"message\":\"file not found\"}");
    }
    uint64_t start = 0;
    uint64_t end = meta.size ? meta.size - 1 : 0;
    bool partial = parse_byte_range(req, meta.size, &start, &end);
    uint64_t to_send = meta.size == 0 ? 0 : (end - start + 1);

    if (partial && fseek(f, (long)start, SEEK_SET) != 0) {
        fclose(f);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"seek_failed\",\"message\":\"seek\"}");
    }

    size_t chunk_sz = 0;
    char *chunk = io_chunk_alloc(&chunk_sz);
    if (!chunk) {
        fclose(f);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"oom\",\"message\":\"oom\"}");
    }
    status_display_set_status(UI_TRANSFERRING);
    /* Always raw bytes — any file type (pdf/docx/zip/…). */
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* ASCII-only filename hint (UTF-8 names are saved by the web UI Blob path). */
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char safe[180];
        size_t si = 0;
        for (const unsigned char *p = (const unsigned char *)base; *p && si + 1 < sizeof(safe); ++p) {
            unsigned char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '_' || c == '-' || c == ' ') {
                safe[si++] = (char)c;
            } else {
                safe[si++] = '_';
            }
        }
        safe[si] = '\0';
        if (!safe[0]) {
            strncpy(safe, "download", sizeof(safe) - 1);
        }
        char disp[220];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", safe);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    if (partial) {
        char cr[80];
        snprintf(cr, sizeof(cr), "bytes %llu-%llu/%llu", (unsigned long long)start, (unsigned long long)end,
                 (unsigned long long)meta.size);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }
    /*
     * Stream raw bytes. Prefer declared size, but stop cleanly on EOF.
     * Always end the chunked response and return ESP_OK if headers were
     * sent — returning ESP_FAIL after a 200 body makes browsers abort
     * (seen as "download failed" on larger pdf/docx).
     */
    bool send_ok = true;
    if (partial || meta.size > 0) {
        uint64_t left = to_send;
        while (left > 0 && send_ok) {
            size_t want = left > (uint64_t)chunk_sz ? chunk_sz : (size_t)left;
            size_t n = fread(chunk, 1, want, f);
            if (n == 0) {
                break; /* unexpected EOF: finish response cleanly */
            }
            if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
                send_ok = false;
                break;
            }
            left -= n;
        }
    } else {
        /* Size unknown/zero: read until EOF (still valid empty file). */
        size_t n;
        while ((n = fread(chunk, 1, chunk_sz, f)) > 0) {
            if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
                send_ok = false;
                break;
            }
        }
    }
    fclose(f);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    status_display_set_status(UI_READY);
    return send_ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t h_download(httpd_req_t *req)
{
    char q[RNDISSTORAGE_MAX_PATH + 1];
    request_path(req, q, sizeof(q));
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(q, path, sizeof(path)) || strcmp(path, "/") == 0) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    return stream_file(req, path);
}

static uint64_t query_u64(httpd_req_t *req, const char *key, uint64_t fallback)
{
    char query[256];
    char val[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return fallback;
    }
    if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
        return fallback;
    }
    return strtoull(val, NULL, 10);
}

static esp_err_t h_stat(httpd_req_t *req)
{
    char q[RNDISSTORAGE_MAX_PATH + 1];
    request_path(req, q, sizeof(q));
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(q, path, sizeof(path))) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    file_meta_t meta;
    storage_stat(path, &meta);
    char body[RNDISSTORAGE_MAX_PATH + 128];
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"exists\":%s,\"is_dir\":%s,\"size\":%llu}",
             meta.exists ? "true" : "false", meta.is_dir ? "true" : "false",
             (unsigned long long)meta.size);
    return send_json(req, 200, body);
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char q[RNDISSTORAGE_MAX_PATH + 1];
    request_path(req, q, sizeof(q));
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(q, path, sizeof(path)) || strcmp(path, "/") == 0) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    uint64_t offset = query_u64(req, "offset", 0);
    if ((uint64_t)req->content_len + offset > RNDISSTORAGE_MAX_FILE_SIZE) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"too_large\",\"message\":\"file too large\"}");
    }
    if (offset > 0) {
        file_meta_t meta;
        storage_stat(path, &meta);
        if (!meta.exists || meta.is_dir || meta.size != offset) {
            return send_json(req, 409,
                             "{\"status\":\"error\",\"error\":\"offset_mismatch\",\"message\":\"resume offset mismatch\"}");
        }
    }
    FILE *f = storage_open_write(path, offset == 0);
    if (!f) {
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"upload_failed\",\"message\":\"cannot open\"}");
    }
    if (offset > 0 && fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"upload_failed\",\"message\":\"seek\"}");
    }
    size_t chunk_sz = 0;
    char *chunk = io_chunk_alloc(&chunk_sz);
    if (!chunk) {
        fclose(f);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"oom\",\"message\":\"oom\"}");
    }
    status_display_set_status(UI_TRANSFERRING);
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)chunk_sz ? (int)chunk_sz : remaining;
        int r = httpd_req_recv(req, chunk, to_read);
        if (r <= 0) {
            fclose(f);
            free(chunk);
            status_display_set_status(UI_READY);
            return send_json(req, 500, "{\"status\":\"error\",\"error\":\"upload_failed\",\"message\":\"recv\"}");
        }
        if (fwrite(chunk, 1, (size_t)r, f) != (size_t)r) {
            fclose(f);
            free(chunk);
            status_display_set_status(UI_READY);
            return send_json(req, 500, "{\"status\":\"error\",\"error\":\"upload_failed\",\"message\":\"write\"}");
        }
        remaining -= r;
    }
    fclose(f);
    free(chunk);
    status_display_set_status(UI_READY);
    return send_json(req, 200, "{\"status\":\"ok\"}");
}

static void dav_set_common_hdrs(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "DAV", "1,2");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
}

static void dav_drain_body(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        return;
    }
    char tmp[256];
    int left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, tmp, left > (int)sizeof(tmp) ? (int)sizeof(tmp) : left);
        if (n <= 0) {
            break;
        }
        left -= n;
    }
}

static void xml_escape_append(char *out, size_t cap, size_t *n, const char *s)
{
    for (; *s && *n + 6 < cap; ++s) {
        if (*s == '&') {
            *n += (size_t)snprintf(out + *n, cap - *n, "&amp;");
        } else if (*s == '<') {
            *n += (size_t)snprintf(out + *n, cap - *n, "&lt;");
        } else if (*s == '>') {
            *n += (size_t)snprintf(out + *n, cap - *n, "&gt;");
        } else if (*s == '"') {
            *n += (size_t)snprintf(out + *n, cap - *n, "&quot;");
        } else {
            out[(*n)++] = *s;
            out[*n] = '\0';
        }
    }
}

/** Encode a virtual path into /dav/... href (percent-encode unsafe bytes). */
static void dav_href(const char *path, bool is_dir, char *out, size_t out_len)
{
    size_t n = 0;
    const char *prefix = "/dav";
    while (*prefix && n + 1 < out_len) {
        out[n++] = *prefix++;
    }
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) {
        if (n + 1 < out_len) {
            out[n++] = '/';
        }
        out[n] = '\0';
        return;
    }
    for (const char *p = path; *p && n + 4 < out_len; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '/') {
            out[n++] = '/';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            n += (size_t)snprintf(out + n, out_len - n, "%%%02X", c);
        }
    }
    if (is_dir && n > 0 && out[n - 1] != '/' && n + 1 < out_len) {
        out[n++] = '/';
    }
    out[n] = '\0';
}

static void http_date(time_t t, char *out, size_t out_len)
{
    if (t <= 0) {
        t = time(NULL);
    }
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, out_len, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
}

static esp_err_t dav_send_prop_response(httpd_req_t *req, const char *path, bool is_dir, uint64_t size,
                                        time_t mtime)
{
    char href[RNDISSTORAGE_MAX_PATH * 3 + 16];
    char date[40];
    char piece[768];
    dav_href(path, is_dir, href, sizeof(href));
    http_date(mtime, date, sizeof(date));
    const char *slash = strrchr(path, '/');
    const char *name = (!path || strcmp(path, "/") == 0) ? "" : (slash ? slash + 1 : path);
    size_t n = 0;
    n += (size_t)snprintf(piece + n, sizeof(piece) - n,
                          "<D:response><D:href>%s</D:href><D:propstat><D:prop>"
                          "<D:displayname>",
                          href);
    xml_escape_append(piece, sizeof(piece), &n, name);
    n += (size_t)snprintf(piece + n, sizeof(piece) - n,
                          "</D:displayname>"
                          "<D:resourcetype>%s</D:resourcetype>"
                          "<D:getcontentlength>%llu</D:getcontentlength>"
                          "<D:getlastmodified>%s</D:getlastmodified>"
                          "<D:supportedlock>"
                          "<D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
                          "<D:locktype><D:write/></D:locktype></D:lockentry>"
                          "</D:supportedlock>"
                          "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>",
                          is_dir ? "<D:collection/>" : "", (unsigned long long)size, date);
    return httpd_resp_send_chunk(req, piece, n);
}

typedef struct {
    httpd_req_t *req;
    char parent[RNDISSTORAGE_MAX_PATH + 1];
    esp_err_t err;
} dav_prop_list_ctx_t;

static bool dav_prop_list_cb(const char *name, bool is_dir, uint64_t size, void *v)
{
    dav_prop_list_ctx_t *c = (dav_prop_list_ctx_t *)v;
    char child[RNDISSTORAGE_MAX_PATH + 64];
    if (strcmp(c->parent, "/") == 0) {
        snprintf(child, sizeof(child), "/%s", name);
    } else {
        snprintf(child, sizeof(child), "%s/%s", c->parent, name);
    }
    child[sizeof(child) - 1] = '\0';
    file_meta_t meta;
    storage_stat(child, &meta);
    if (dav_send_prop_response(c->req, child, is_dir, size, meta.mtime) != ESP_OK) {
        c->err = ESP_FAIL;
        return false;
    }
    return true;
}

static int dav_depth(httpd_req_t *req)
{
    char depth[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "Depth", depth, sizeof(depth)) != ESP_OK) {
        /* RFC default is infinity; keep embedded RAM safe → one level. */
        return 1;
    }
    if (strcmp(depth, "0") == 0) {
        return 0;
    }
    /* "1" and "infinity" → children only (Explorer walks trees itself). */
    return 1;
}

static bool dav_parse_destination(httpd_req_t *req, char *out, size_t out_len)
{
    char dest[512];
    if (httpd_req_get_hdr_value_str(req, "Destination", dest, sizeof(dest)) != ESP_OK) {
        return false;
    }
    const char *p = dest;
    if (strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0) {
        p = strchr(p + 8, '/');
        if (!p) {
            return false;
        }
    }
    if (strncmp(p, "/dav", 4) == 0) {
        p += 4;
    }
    if (*p == '\0') {
        p = "/";
    }
    char decoded[sizeof(dest)];
    strncpy(decoded, p, sizeof(decoded) - 1);
    decoded[sizeof(decoded) - 1] = '\0';
    url_decode_inplace(decoded);
    return normalize_path(decoded, out, out_len);
}

static esp_err_t dav_put(httpd_req_t *req, const char *path)
{
    if (req->content_len > (int)RNDISSTORAGE_MAX_FILE_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
        return ESP_FAIL;
    }
    FILE *f = storage_open_write(path, true);
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write");
        return ESP_FAIL;
    }
    size_t chunk_sz = 0;
    char *chunk = io_chunk_alloc(&chunk_sz);
    if (!chunk) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    status_display_set_status(UI_TRANSFERRING);
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int to_read = remaining > (int)chunk_sz ? (int)chunk_sz : remaining;
        int r = httpd_req_recv(req, chunk, to_read);
        if (r <= 0) {
            ok = false;
            break;
        }
        if (fwrite(chunk, 1, (size_t)r, f) != (size_t)r) {
            ok = false;
            break;
        }
        remaining -= r;
    }
    fclose(f);
    free(chunk);
    status_display_set_status(UI_READY);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write");
        return ESP_FAIL;
    }
    dav_set_common_hdrs(req);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_lock(httpd_req_t *req, const char *path)
{
    (void)path;
    dav_drain_body(req);
    static const char *body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:prop xmlns:D=\"DAV:\">"
        "<D:lockdiscovery><D:activelock>"
        "<D:locktype><D:write/></D:locktype>"
        "<D:lockscope><D:exclusive/></D:lockscope>"
        "<D:depth>Infinity</D:depth>"
        "<D:owner><D:href>RNDISStorage</D:href></D:owner>"
        "<D:timeout>Second-600</D:timeout>"
        "<D:locktoken><D:href>opaquelocktoken:rndisstorage-lock</D:href></D:locktoken>"
        "</D:activelock></D:lockdiscovery></D:prop>";
    dav_set_common_hdrs(req);
    httpd_resp_set_hdr(req, "Lock-Token", "<opaquelocktoken:rndisstorage-lock>");
    httpd_resp_set_type(req, "application/xml");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_dav(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *raw = "/";
    if (strncmp(uri, "/dav", 4) == 0) {
        raw = (uri[4] == '\0' || (uri[4] == '/' && uri[5] == '\0')) ? "/" : uri + 4;
    }
    char path[RNDISSTORAGE_MAX_PATH + 1];
    char path_dec[RNDISSTORAGE_MAX_PATH + 64];
    strncpy(path_dec, raw, sizeof(path_dec) - 1);
    path_dec[sizeof(path_dec) - 1] = '\0';
    url_decode_inplace(path_dec);
    if (!normalize_path(path_dec, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    if (req->method == HTTP_OPTIONS) {
        dav_set_common_hdrs(req);
        httpd_resp_set_hdr(req, "Allow",
                           "OPTIONS,HEAD,GET,PUT,DELETE,PROPFIND,PROPPATCH,MKCOL,COPY,MOVE,LOCK,UNLOCK");
        return httpd_resp_send(req, NULL, 0);
    }

    if (req->method == HTTP_LOCK) {
        return dav_lock(req, path);
    }
    if (req->method == HTTP_UNLOCK) {
        dav_drain_body(req);
        dav_set_common_hdrs(req);
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    if (req->method == HTTP_PROPPATCH) {
        dav_drain_body(req);
        dav_set_common_hdrs(req);
        httpd_resp_set_status(req, "207 Multi-Status");
        httpd_resp_set_type(req, "application/xml");
        char href[RNDISSTORAGE_MAX_PATH * 3 + 16];
        char xml[RNDISSTORAGE_MAX_PATH * 3 + 256];
        dav_href(path, storage_is_directory(path), href, sizeof(href));
        snprintf(xml, sizeof(xml),
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                 "<D:multistatus xmlns:D=\"DAV:\">"
                 "<D:response><D:href>%s</D:href>"
                 "<D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
                 "</D:response></D:multistatus>",
                 href);
        return httpd_resp_send(req, xml, HTTPD_RESP_USE_STRLEN);
    }

    if (req->method == HTTP_MKCOL) {
        dav_drain_body(req);
        file_meta_t meta;
        storage_stat(path, &meta);
        if (meta.exists) {
            httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "exists");
            return ESP_FAIL;
        }
        if (!storage_mkdir(path)) {
            httpd_resp_send_custom_err(req, "409 Conflict", "parent missing");
            return ESP_FAIL;
        }
        dav_set_common_hdrs(req);
        httpd_resp_set_status(req, "201 Created");
        return httpd_resp_send(req, NULL, 0);
    }

    if (req->method == HTTP_DELETE) {
        if (strcmp(path, "/") == 0) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "root");
            return ESP_FAIL;
        }
        if (storage_remove_recursive(path)) {
            dav_set_common_hdrs(req);
            httpd_resp_set_status(req, "204 No Content");
            return httpd_resp_send(req, NULL, 0);
        }
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_MOVE || req->method == HTTP_COPY) {
        char dest[RNDISSTORAGE_MAX_PATH + 1];
        if (!dav_parse_destination(req, dest, sizeof(dest)) || strcmp(dest, "/") == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "destination");
            return ESP_FAIL;
        }
        dav_drain_body(req);
        bool ok = false;
        if (req->method == HTTP_MOVE) {
            ok = storage_rename(path, dest);
        } else {
            /* Best-effort COPY for files only (Explorer rarely needs deep copy server-side). */
            file_meta_t meta;
            storage_stat(path, &meta);
            if (meta.exists && !meta.is_dir) {
                FILE *in = storage_open_read(path);
                FILE *out = storage_open_write(dest, true);
                if (in && out) {
                    size_t chunk_sz = 0;
                    char *chunk = io_chunk_alloc(&chunk_sz);
                    ok = chunk != NULL;
                    if (ok) {
                        size_t n;
                        while ((n = fread(chunk, 1, chunk_sz, in)) > 0) {
                            if (fwrite(chunk, 1, n, out) != n) {
                                ok = false;
                                break;
                            }
                        }
                        free(chunk);
                    }
                }
                if (in) {
                    fclose(in);
                }
                if (out) {
                    fclose(out);
                }
            }
        }
        if (!ok) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "move/copy");
            return ESP_FAIL;
        }
        dav_set_common_hdrs(req);
        httpd_resp_set_status(req, "201 Created");
        return httpd_resp_send(req, NULL, 0);
    }

    if (req->method == HTTP_PUT) {
        return dav_put(req, path);
    }

    if (req->method == HTTP_HEAD || req->method == HTTP_GET) {
        file_meta_t meta;
        storage_stat(path, &meta);
        if (!meta.exists || meta.is_dir) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
            return ESP_FAIL;
        }
        if (req->method == HTTP_HEAD) {
            char len[32];
            char date[40];
            snprintf(len, sizeof(len), "%llu", (unsigned long long)meta.size);
            http_date(meta.mtime, date, sizeof(date));
            dav_set_common_hdrs(req);
            httpd_resp_set_type(req, "application/octet-stream");
            httpd_resp_set_hdr(req, "Content-Length", len);
            httpd_resp_set_hdr(req, "Last-Modified", date);
            return httpd_resp_send(req, NULL, 0);
        }
        dav_set_common_hdrs(req);
        return stream_file(req, path);
    }

    if (req->method == HTTP_PROPFIND) {
        dav_drain_body(req);
        file_meta_t meta;
        storage_stat(path, &meta);
        if (!meta.exists) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
            return ESP_FAIL;
        }
        int depth = dav_depth(req);
        dav_set_common_hdrs(req);
        httpd_resp_set_status(req, "207 Multi-Status");
        httpd_resp_set_type(req, "application/xml; charset=utf-8");
        if (httpd_resp_sendstr_chunk(req, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                          "<D:multistatus xmlns:D=\"DAV:\">") != ESP_OK) {
            return ESP_FAIL;
        }
        if (dav_send_prop_response(req, path, meta.is_dir, meta.size, meta.mtime) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        if (depth >= 1 && meta.is_dir) {
            dav_prop_list_ctx_t ctx;
            ctx.req = req;
            ctx.err = ESP_OK;
            strncpy(ctx.parent, path, sizeof(ctx.parent) - 1);
            ctx.parent[sizeof(ctx.parent) - 1] = '\0';
            storage_list(path, dav_prop_list_cb, &ctx);
            if (ctx.err != ESP_OK) {
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_FAIL;
            }
        }
        if (httpd_resp_sendstr_chunk(req, "</D:multistatus>") != ESP_OK) {
            return ESP_FAIL;
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method");
    return ESP_FAIL;
}

esp_err_t http_webdav_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = RNDISSTORAGE_HTTP_PORT;
    config.max_uri_handlers = 18;
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* Extra stack for WebDAV XML + large path buffers during Explorer walks. */
    config.stack_size = 16384;
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 60;
    config.lru_purge_enable = true;
    /* Windows WebClient opens several parallel sockets when copying trees. */
    config.max_open_sockets = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/api/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/api/list", .method = HTTP_GET, .handler = h_list},
        {.uri = "/api/mkdir", .method = HTTP_POST, .handler = h_mkdir},
        {.uri = "/api/touch", .method = HTTP_POST, .handler = h_touch},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/rename", .method = HTTP_POST, .handler = h_rename},
        {.uri = "/api/stat", .method = HTTP_GET, .handler = h_stat},
        {.uri = "/api/download", .method = HTTP_GET, .handler = h_download},
        {.uri = "/api/upload", .method = HTTP_PUT, .handler = h_upload},
        {.uri = "/api/pivot/probe", .method = HTTP_GET, .handler = h_pivot_probe},
        {.uri = "/dav", .method = HTTP_ANY, .handler = h_dav},
        {.uri = "/dav/*", .method = HTTP_ANY, .handler = h_dav},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }
    ESP_LOGI(TAG, "HTTP/WebDAV on port %d (Plug&Play)", RNDISSTORAGE_HTTP_PORT);
    return ESP_OK;
}
