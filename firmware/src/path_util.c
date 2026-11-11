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
    for (const unsigned char *p = (const unsigned char *)component; *p; ++p) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        if (c == '/' || c == '\\' || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
            c == '?' || c == '*') {
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

bool is_valid_utf8(const char *s)
{
    if (!s) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p <= 0x7F) {
            p++;
            continue;
        }
        if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80 || *p < 0xC2) {
                return false;
            }
            p += 2;
            continue;
        }
        if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) {
                return false;
            }
            p += 3;
            continue;
        }
        if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) {
                return false;
            }
            p += 4;
            continue;
        }
        return false;
    }
    return true;
}

/** CP437 glyphs for bytes 0x80..0xFF (IBM PC). */
static const unsigned short CP437_HI[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, /* 80 */
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, /* 88 */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, /* 90 */
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, /* 98 */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, /* A0 */
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, /* A8 */
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, /* B0 */
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, /* B8 */
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, /* C0 */
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, /* C8 */
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, /* D0 */
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, /* D8 */
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, /* E0 */
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, /* E8 */
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, /* F0 */
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0, /* F8 */
};

static int utf8_decode(const unsigned char *p, unsigned *cp, size_t *n)
{
    if (*p <= 0x7F) {
        *cp = *p;
        *n = 1;
        return 1;
    }
    if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
        *n = 2;
        return 1;
    }
    if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *n = 3;
        return 1;
    }
    if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *n = 4;
        return 1;
    }
    return 0;
}

static size_t utf8_encode(unsigned cp, char *out, size_t out_len, size_t o)
{
    if (cp <= 0x7F) {
        if (o + 1 >= out_len) {
            return (size_t)-1;
        }
        out[o++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (o + 2 >= out_len) {
            return (size_t)-1;
        }
        out[o++] = (char)(0xC0 | (cp >> 6));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (o + 3 >= out_len) {
            return (size_t)-1;
        }
        out[o++] = (char)(0xE0 | (cp >> 12));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (o + 1 >= out_len) {
            return (size_t)-1;
        }
        out[o++] = '?';
    }
    return o;
}

static bool is_spanish_latin1_letter_byte(unsigned b)
{
    switch (b) {
    case 0xE1: /* á */
    case 0xE9: /* é */
    case 0xED: /* í */
    case 0xF3: /* ó */
    case 0xFA: /* ú */
    case 0xF1: /* ñ */
    case 0xFC: /* ü */
    case 0xC1: /* Á */
    case 0xC9: /* É */
    case 0xCD: /* Í */
    case 0xD3: /* Ó */
    case 0xDA: /* Ú */
    case 0xD1: /* Ñ */
    case 0xDC: /* Ü */
    case 0xE0:
    case 0xE2:
    case 0xE4:
    case 0xE7:
    case 0xEA:
    case 0xEB:
    case 0xEC:
    case 0xEE:
    case 0xEF:
    case 0xF2:
    case 0xF4:
    case 0xF6:
    case 0xF9:
    case 0xFB:
        return true;
    default:
        return false;
    }
}

/** Undo CP437 mis-decode of Latin-1: Marφa→María, Garo±a→Garoña. */
static bool undo_cp437_mojibake_utf8(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p) {
        unsigned cp;
        size_t n = 0;
        if (!utf8_decode(p, &cp, &n)) {
            return false;
        }
        p += n;
        /*
         * Only remap CP437 glyphs back to Latin-1 when the OEM byte is a
         * Spanish letter (í/ñ/…). Never remap when `cp` is already that letter
         * (CP437 also draws í at 0xA1 — must not turn real í into ¡).
         */
        for (int i = 0; i < 128; i++) {
            unsigned oem = (unsigned)(0x80 + i);
            if (CP437_HI[i] == cp && CP437_HI[i] != oem && is_spanish_latin1_letter_byte(oem)) {
                cp = oem;
                break;
            }
        }
        o = utf8_encode(cp, out, out_len, o);
        if (o == (size_t)-1) {
            return false;
        }
    }
    if (o >= out_len) {
        return false;
    }
    out[o] = '\0';
    return true;
}

static bool latin1_bytes_to_utf8(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        o = utf8_encode(*p, out, out_len, o);
        if (o == (size_t)-1) {
            return false;
        }
    }
    out[o] = '\0';
    return true;
}

bool fs_name_to_utf8(const char *fs_name, char *out, size_t out_len)
{
    if (!fs_name || !out || out_len == 0) {
        return false;
    }

    char step[RNDISSTORAGE_MAX_PATH * 2 + 4];
    if (!is_valid_utf8(fs_name)) {
        /* Raw Latin-1 bytes from ANSI FatFs. */
        if (!latin1_bytes_to_utf8(fs_name, step, sizeof(step))) {
            return false;
        }
    } else {
        if (strlen(fs_name) >= sizeof(step)) {
            return false;
        }
        memcpy(step, fs_name, strlen(fs_name) + 1);
    }

    /* Always try CP437 mojibake repair (φ/± → í/ñ). Harmless if already correct. */
    char fixed[RNDISSTORAGE_MAX_PATH * 2 + 4];
    if (!undo_cp437_mojibake_utf8(step, fixed, sizeof(fixed))) {
        if (strlen(step) >= out_len) {
            return false;
        }
        memcpy(out, step, strlen(step) + 1);
        return true;
    }
    if (strlen(fixed) >= out_len) {
        return false;
    }
    memcpy(out, fixed, strlen(fixed) + 1);
    return true;
}

bool utf8_to_fs_latin1(const char *utf8, char *out, size_t out_len)
{
    if (!utf8 || !out || out_len == 0) {
        return false;
    }
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        unsigned cp;
        size_t n = 0;
        if (!utf8_decode(p, &cp, &n)) {
            cp = '?';
            n = 1;
        }
        p += n;
        if (o + 1 >= out_len) {
            return false;
        }
        out[o++] = (cp <= 0xFF) ? (char)cp : '?';
    }
    out[o] = '\0';
    return true;
}

/** Build CP437-mojibake UTF-8 form for fallback open of corrupted LFNs. */
bool utf8_to_cp437_mojibake(const char *utf8, char *out, size_t out_len)
{
    if (!utf8 || !out || out_len == 0) {
        return false;
    }
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        unsigned cp;
        size_t n = 0;
        if (!utf8_decode(p, &cp, &n)) {
            return false;
        }
        p += n;
        if (cp >= 0x80 && cp <= 0xFF) {
            cp = CP437_HI[cp - 0x80];
        }
        o = utf8_encode(cp, out, out_len, o);
        if (o == (size_t)-1) {
            return false;
        }
    }
    out[o] = '\0';
    return true;
}
