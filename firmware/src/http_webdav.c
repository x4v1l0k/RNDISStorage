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

#include "lwip/inet.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

/** Prefer larger buffered I/O; allocated once per transfer on the heap. */
static char *io_chunk_alloc(void)
{
    char *p = (char *)heap_caps_malloc(RNDISSTORAGE_IO_CHUNK, MALLOC_CAP_DEFAULT);
    if (!p) {
        p = (char *)malloc(RNDISSTORAGE_IO_CHUNK);
    }
    return p;
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

static bool query_path(httpd_req_t *req, char *out, size_t out_len)
{
    if (out_len == 0) {
        return false;
    }
    strncpy(out, "/", out_len - 1);
    out[out_len - 1] = '\0';
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return true;
    }
    if (httpd_query_key_value(query, "path", out, out_len) != ESP_OK) {
        strncpy(out, "/", out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
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
    char item[320];
    // Escape minimal JSON string specials in file names
    char esc[200];
    size_t ei = 0;
    for (const char *p = name; *p && ei + 2 < sizeof(esc); ++p) {
        if (*p == '"' || *p == '\\') {
            esc[ei++] = '\\';
        }
        if ((uint8_t)*p < 0x20) {
            continue;
        }
        esc[ei++] = *p;
    }
    esc[ei] = '\0';

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
    httpd_resp_set_type(req, "application/json");
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
    httpd_resp_set_type(req, "text/html");
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
    query_path(req, q, sizeof(q));
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
    bool ok = storage_remove(path);
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

static esp_err_t stream_file(httpd_req_t *req, const char *path)
{
    FILE *f = storage_open_read(path);
    if (!f) {
        return send_json(req, 404, "{\"status\":\"error\",\"error\":\"not_found\",\"message\":\"file not found\"}");
    }
    char *chunk = io_chunk_alloc();
    if (!chunk) {
        fclose(f);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"oom\",\"message\":\"oom\"}");
    }
    setvbuf(f, NULL, _IOFBF, RNDISSTORAGE_IO_CHUNK);
    status_display_set_status(UI_TRANSFERRING);
    httpd_resp_set_type(req, "application/octet-stream");
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(chunk, 1, RNDISSTORAGE_IO_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    status_display_set_status(UI_READY);
    return err;
}

static esp_err_t h_download(httpd_req_t *req)
{
    char q[RNDISSTORAGE_MAX_PATH + 1];
    query_path(req, q, sizeof(q));
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(q, path, sizeof(path)) || strcmp(path, "/") == 0) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    return stream_file(req, path);
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char q[RNDISSTORAGE_MAX_PATH + 1];
    query_path(req, q, sizeof(q));
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(q, path, sizeof(path)) || strcmp(path, "/") == 0) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"bad_path\",\"message\":\"invalid path\"}");
    }
    if (req->content_len > (int)RNDISSTORAGE_MAX_FILE_SIZE) {
        return send_json(req, 400, "{\"status\":\"error\",\"error\":\"too_large\",\"message\":\"file too large\"}");
    }
    FILE *f = storage_open_write(path, true);
    if (!f) {
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"upload_failed\",\"message\":\"cannot open\"}");
    }
    char *chunk = io_chunk_alloc();
    if (!chunk) {
        fclose(f);
        return send_json(req, 500, "{\"status\":\"error\",\"error\":\"oom\",\"message\":\"oom\"}");
    }
    setvbuf(f, NULL, _IOFBF, RNDISSTORAGE_IO_CHUNK);
    status_display_set_status(UI_TRANSFERRING);
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > RNDISSTORAGE_IO_CHUNK ? RNDISSTORAGE_IO_CHUNK : remaining;
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

typedef struct {
    char *xml;
    size_t *pn;
    size_t cap;
    char parent[RNDISSTORAGE_MAX_PATH + 1];
} dav_list_ctx_t;

static bool dav_list_cb(const char *name, bool is_dir, uint64_t size, void *v)
{
    dav_list_ctx_t *c = (dav_list_ctx_t *)v;
    char child[RNDISSTORAGE_MAX_PATH + 64];
    if (strcmp(c->parent, "/") == 0) {
        snprintf(child, sizeof(child), "/%s", name);
    } else {
        snprintf(child, sizeof(child), "%s/%s", c->parent, name);
    }
    child[sizeof(child) - 1] = '\0';
    char piece[320];
    int m = snprintf(piece, sizeof(piece),
                     "<D:response><D:href>/dav%s%s</D:href><D:propstat><D:prop>"
                     "<D:resourcetype>%s</D:resourcetype>"
                     "<D:getcontentlength>%llu</D:getcontentlength>"
                     "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>",
                     child, is_dir ? "/" : "", is_dir ? "<D:collection/>" : "", (unsigned long long)size);
    if (*c->pn + (size_t)m + 20 >= c->cap) {
        return false;
    }
    memcpy(c->xml + *c->pn, piece, (size_t)m);
    *c->pn += (size_t)m;
    return true;
}

static esp_err_t h_dav(httpd_req_t *req)
{
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "DAV", "1, 2");
        httpd_resp_set_hdr(req, "Allow", "OPTIONS, PROPFIND, GET, PUT, DELETE, MKCOL");
        return httpd_resp_send(req, NULL, 0);
    }

    const char *uri = req->uri;
    const char *raw = "/";
    if (strncmp(uri, "/dav", 4) == 0) {
        raw = (uri[4] == '\0' || (uri[4] == '/' && uri[5] == '\0')) ? "/" : uri + 4;
    }
    char path[RNDISSTORAGE_MAX_PATH + 1];
    if (!normalize_path(raw, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        file_meta_t meta;
        storage_stat(path, &meta);
        if (meta.exists && !meta.is_dir) {
            return stream_file(req, path);
        }
    }
    if (req->method == HTTP_DELETE) {
        if (storage_remove(path)) {
            httpd_resp_set_status(req, "204 No Content");
            return httpd_resp_send(req, NULL, 0);
        }
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
        return ESP_FAIL;
    }
    if (req->method == HTTP_PUT) {
        FILE *f = storage_open_write(path, true);
        if (!f) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write");
            return ESP_FAIL;
        }
        char *chunk = io_chunk_alloc();
        if (!chunk) {
            fclose(f);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            return ESP_FAIL;
        }
        setvbuf(f, NULL, _IOFBF, RNDISSTORAGE_IO_CHUNK);
        status_display_set_status(UI_TRANSFERRING);
        int remaining = req->content_len;
        bool ok = true;
        while (remaining > 0) {
            int to_read = remaining > RNDISSTORAGE_IO_CHUNK ? RNDISSTORAGE_IO_CHUNK : remaining;
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
        httpd_resp_set_status(req, "201 Created");
        return httpd_resp_send(req, NULL, 0);
    }

    file_meta_t meta;
    if (!storage_stat(path, &meta) || !meta.exists) {
        if (req->content_len == 0 && storage_mkdir(path)) {
            httpd_resp_set_status(req, "201 Created");
            return httpd_resp_send(req, NULL, 0);
        }
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
        return ESP_FAIL;
    }

    char *xml = malloc(8192);
    if (!xml) {
        return ESP_FAIL;
    }
    size_t n = (size_t)snprintf(xml, 8192,
                                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                "<D:multistatus xmlns:D=\"DAV:\">"
                                "<D:response><D:href>/dav%s</D:href><D:propstat><D:prop>"
                                "<D:resourcetype>%s</D:resourcetype>"
                                "<D:getcontentlength>%llu</D:getcontentlength>"
                                "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>",
                                strcmp(path, "/") == 0 ? "/" : path, meta.is_dir ? "<D:collection/>" : "",
                                (unsigned long long)meta.size);
    if (meta.is_dir) {
        dav_list_ctx_t ctx;
        ctx.xml = xml;
        ctx.pn = &n;
        ctx.cap = 8192;
        strncpy(ctx.parent, path, sizeof(ctx.parent) - 1);
        ctx.parent[sizeof(ctx.parent) - 1] = '\0';
        storage_list(path, dav_list_cb, &ctx);
    }
    memcpy(xml + n, "</D:multistatus>", 17);
    n += 16;
    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml");
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    esp_err_t err = httpd_resp_send(req, xml, n);
    free(xml);
    return err;
}

esp_err_t http_webdav_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = RNDISSTORAGE_HTTP_PORT;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 12288;
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 60;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/api/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/api/list", .method = HTTP_GET, .handler = h_list},
        {.uri = "/api/mkdir", .method = HTTP_POST, .handler = h_mkdir},
        {.uri = "/api/touch", .method = HTTP_POST, .handler = h_touch},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/rename", .method = HTTP_POST, .handler = h_rename},
        {.uri = "/api/download", .method = HTTP_GET, .handler = h_download},
        {.uri = "/api/upload", .method = HTTP_PUT, .handler = h_upload},
        {.uri = "/api/pivot/probe", .method = HTTP_GET, .handler = h_pivot_probe},
        {.uri = "/dav", .method = HTTP_ANY, .handler = h_dav},
        {.uri = "/dav/*", .method = HTTP_ANY, .handler = h_dav},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }
    ESP_LOGI(TAG, "HTTP/WebDAV on port %d", RNDISSTORAGE_HTTP_PORT);
    return ESP_OK;
}
