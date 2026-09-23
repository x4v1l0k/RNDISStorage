#include "path_util.h"
#include "board_config.h"

#include <ctype.h>
#include <string.h>

bool is_safe_file_name_component(const char *component)
{
    if (!component || !*component) {
        return false;
    }
    if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
        return false;
    }
    for (const char *p = component; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == ' ';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool normalize_path(const char *input, char *out, size_t out_len)
{
    if (!input || !out || out_len < 2) {
        return false;
    }
    if (strchr(input, '\\') != NULL) {
        return false;
    }
    if (strlen(input) >= 2 && isalpha((unsigned char)input[0]) && input[1] == ':') {
        return false;
    }
    if (strlen(input) > RNDISSTORAGE_MAX_PATH) {
        return false;
    }

    char tmp[RNDISSTORAGE_MAX_PATH + 1];
    strncpy(tmp, input, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char assembled[RNDISSTORAGE_MAX_PATH + 1];
    assembled[0] = '/';
    assembled[1] = '\0';
    size_t a_len = 1;

    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok != NULL; tok = strtok_r(NULL, "/", &save)) {
        if (tok[0] == '\0' || strcmp(tok, ".") == 0) {
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            return false;
        }
        if (!is_safe_file_name_component(tok)) {
            return false;
        }
        size_t t_len = strlen(tok);
        size_t need = (a_len > 1 ? 1 : 0) + t_len;
        if (a_len + need >= sizeof(assembled)) {
            return false;
        }
        if (a_len > 1) {
            assembled[a_len++] = '/';
        }
        memcpy(assembled + a_len, tok, t_len);
        a_len += t_len;
        assembled[a_len] = '\0';
    }

    if (a_len >= out_len) {
        return false;
    }
    memcpy(out, assembled, a_len + 1);
    return true;
}
