/* tb_subs.c - subtitle parsing port (cleanSubtitleText / parseVttOrSrt / parseAss). */
#include "tb_subs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
/* On Windows, paths in this project are UTF-8. The CRT's fopen() interprets its
 * argument in the (legacy) ANSI code page, so it cannot open files whose names
 * contain characters outside that code page (e.g. Japanese titles). Convert
 * UTF-8 -> UTF-16 and use _wfopen so any Unicode path opens correctly. */
static FILE *tb_fopen(const char *path, const char *mode) {
    wchar_t wpath[4096], wmode[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096) <= 0) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16) <= 0) return NULL;
    return _wfopen(wpath, wmode);
}
#else
#define tb_fopen fopen
#endif

/* Portable replacements (strdup / strcasecmp are POSIX, missing on MSVC/C11). */
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
static int ci_ncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]); if (ca != cb) return ca - cb; if (!ca) break; } return 0;
}
static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) { int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b); if (ca != cb) return ca - cb; if (!ca) return 0; }
}
#define strdup       xstrdup
#define strncasecmp  ci_ncmp
#define strcasecmp   ci_cmp

void tb_cue_list_init(tb_cue_list *l) { l->data = NULL; l->count = 0; l->cap = 0; l->label[0] = '\0'; }
void tb_cue_list_free(tb_cue_list *l) {
    for (size_t i = 0; i < l->count; i++) free(l->data[i].text);
    free(l->data); l->data = NULL; l->count = l->cap = 0;
}
void tb_cue_list_push(tb_cue_list *l, double start, double end, const char *text) {
    if (!text || !*text) return;
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        tb_cue *nd = realloc(l->data, nc * sizeof(*nd));
        if (!nd) return; l->data = nd; l->cap = nc;
    }
    l->data[l->count].start = start; l->data[l->count].end = end;
    l->data[l->count].text = strdup(text);
    l->count++;
}
static int cmp_cue(const void *a, const void *b) {
    const tb_cue *x = a, *y = b;
    if (x->start < y->start) return -1; if (x->start > y->start) return 1;
    if (x->end < y->end) return -1; if (x->end > y->end) return 1; return 0;
}
void tb_cue_list_sort(tb_cue_list *l) { qsort(l->data, l->count, sizeof(tb_cue), cmp_cue); }

/* ---------- text cleaning ---------- */
static void append_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) { *cap = *cap ? *cap * 2 : 64; *buf = realloc(*buf, *cap); }
    (*buf)[(*len)++] = c;
}
static void append_str(char **buf, size_t *len, size_t *cap, const char *s) {
    while (*s) append_char(buf, len, cap, *s++);
}

/* minimal HTML entity decode for the common cases used by subtitles */
static void decode_entity(const char *e, size_t n, char **buf, size_t *len, size_t *cap) {
    char tmp[16]; if (n >= sizeof tmp) { append_char(buf, len, cap, '&'); return; }
    memcpy(tmp, e, n); tmp[n] = '\0';
    if (tmp[0] == '#') {
        long code = (tmp[1] == 'x' || tmp[1] == 'X') ? strtol(tmp + 2, NULL, 16) : strtol(tmp + 1, NULL, 10);
        if (code > 0 && code < 128) { append_char(buf, len, cap, (char)code); return; }
        /* encode small UTF-8 for >127 */
        if (code >= 128 && code < 0x800) { append_char(buf, len, cap, (char)(0xC0 | (code >> 6))); append_char(buf, len, cap, (char)(0x80 | (code & 0x3F))); return; }
        if (code >= 0x800 && code < 0x10000) { append_char(buf, len, cap, (char)(0xE0 | (code >> 12))); append_char(buf, len, cap, (char)(0x80 | ((code >> 6) & 0x3F))); append_char(buf, len, cap, (char)(0x80 | (code & 0x3F))); return; }
        return;
    }
    if (strcmp(tmp, "amp") == 0) append_char(buf, len, cap, '&');
    else if (strcmp(tmp, "lt") == 0) append_char(buf, len, cap, '<');
    else if (strcmp(tmp, "gt") == 0) append_char(buf, len, cap, '>');
    else if (strcmp(tmp, "quot") == 0) append_char(buf, len, cap, '"');
    else if (strcmp(tmp, "apos") == 0) append_char(buf, len, cap, '\'');
    else if (strcmp(tmp, "nbsp") == 0) append_char(buf, len, cap, ' ');
    else { append_char(buf, len, cap, '&'); append_str(buf, len, cap, tmp); append_char(buf, len, cap, ';'); }
}

char *tb_clean_subtitle_text(const char *raw) {
    if (!raw) return strdup("");
    char *buf = NULL; size_t len = 0, cap = 0;
    const char *p = raw;
    while (*p) {
        if (*p == '{') {                       /* ASS override block {...} */
            const char *q = strchr(p, '}'); if (!q) break; p = q + 1; continue;
        }
        if (*p == '\\' && (p[1] == 'N' || p[1] == 'n')) { append_char(&buf, &len, &cap, '\n'); p += 2; continue; }
        if (*p == '<') {
            /* <br> -> newline, any other <...> dropped */
            if (strncasecmp(p, "<br", 3) == 0) append_char(&buf, &len, &cap, '\n');
            const char *q = strchr(p, '>'); if (!q) break; p = q + 1; continue;
        }
        if (*p == '&') {
            const char *q = strchr(p, ';');
            if (q && q - p <= 12) { decode_entity(p + 1, (size_t)(q - p - 1), &buf, &len, &cap); p = q + 1; continue; }
        }
        append_char(&buf, &len, &cap, *p++);
    }
    if (!buf) return strdup("");
    buf[len] = '\0';

    /* trim each line, collapse 3+ newlines, overall trim */
    char *out = malloc(len + 1); size_t o = 0;
    char *line = buf; size_t nl_run = 0;
    char *save = buf;
    for (char *s = buf; ; s++) {
        if (*s == '\n' || *s == '\0') {
            /* trim [line, s) */
            char *a = line; char *b = s;
            while (a < b && isspace((unsigned char)*a)) a++;
            while (b > a && isspace((unsigned char)b[-1])) b--;
            if (b > a) {
                if (o > 0) { out[o++] = '\n'; }
                memcpy(out + o, a, (size_t)(b - a)); o += (size_t)(b - a);
                nl_run = 0;
            } else {
                /* blank line -> at most one separating newline kept implicitly */
                (void)nl_run;
            }
            if (*s == '\0') break;
            line = s + 1;
        }
    }
    (void)save;
    out[o] = '\0';
    free(buf);
    /* final trim */
    char *a = out; while (*a && isspace((unsigned char)*a)) a++;
    size_t L = strlen(a); while (L && isspace((unsigned char)a[L-1])) a[--L] = '\0';
    char *res = strdup(a); free(out);
    return res;
}

/* ---------- time parse ---------- */
double tb_parse_subtitle_time(const char *raw) {
    if (!raw) return NAN;
    /* skip leading spaces */
    while (*raw && isspace((unsigned char)*raw)) raw++;
    int parts[3] = {0,0,0}, np = 0; double frac = 0;
    char buf[64]; size_t bi = 0;
    /* copy until whitespace, normalising comma->dot */
    while (*raw && !isspace((unsigned char)*raw) && bi < sizeof buf - 1) {
        char c = *raw++; if (c == ',') c = '.'; buf[bi++] = c;
    }
    buf[bi] = '\0';
    /* split on ':' then '.' */
    char *dot = strchr(buf, '.');
    if (dot) { *dot = '\0'; frac = atof(dot + 1) / pow(10, strlen(dot + 1)); }
    char *tok = strtok(buf, ":");
    char *toks[4] = {0,0,0,0}; int nt = 0;
    while (tok && nt < 4) { toks[nt++] = tok; tok = strtok(NULL, ":"); }
    if (nt < 2 || nt > 3) return NAN;
    /* toks order: [H] M S */
    int off = (nt == 3) ? 0 : 1;
    for (int i = 0; i < nt; i++) parts[off + i] = atoi(toks[i]);
    return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2] + frac;
}

/* ---------- ext helpers ---------- */
static const char *file_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}
static bool ext_is(const char *name, const char *ext) {
    const char *e = file_ext(name); return strcasecmp(e, ext) == 0;
}
bool tb_is_subtitle_path(const char *path) {
    return ext_is(path, "srt") || ext_is(path, "vtt") || ext_is(path, "ass") || ext_is(path, "ssa");
}

/* ---------- SRT / VTT ---------- */
static bool block_is_header(const char *b) {
    return strncasecmp(b, "WEBVTT", 6) == 0 || strncasecmp(b, "NOTE", 4) == 0
        || strncasecmp(b, "STYLE", 5) == 0 || strncasecmp(b, "REGION", 6) == 0;
}

static void process_srt_block(char *b, tb_cue_list *out) {
    while (*b && isspace((unsigned char)*b)) b++;
    if (!*b || block_is_header(b)) return;
    char *arrow = strstr(b, "-->");
    if (!arrow) return;
    /* the arrow's line */
    char *ls = arrow; while (ls > b && ls[-1] != '\n') ls--;
    char *le = strchr(arrow, '\n');
    char *body = le ? le + 1 : NULL;
    char arrowLine[160]; size_t al = (size_t)((le ? le : arrow + strlen(arrow)) - ls);
    if (al >= sizeof arrowLine) al = sizeof arrowLine - 1;
    memcpy(arrowLine, ls, al); arrowLine[al] = '\0';
    char *sep = strstr(arrowLine, "-->");
    if (!sep) return;
    *sep = '\0';
    double start = tb_parse_subtitle_time(arrowLine);
    double end = tb_parse_subtitle_time(sep + 3);   /* parser stops at first whitespace, ignoring cue settings */
    if (!(isfinite(start) && isfinite(end) && end > start)) return;
    char *clean = tb_clean_subtitle_text(body ? body : "");
    if (clean[0]) tb_cue_list_push(out, start, end, clean);
    free(clean);
}

static bool parse_vtt_srt(const char *text, tb_cue_list *out) {
    char *copy = strdup(text);
    char *src = copy;
    if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) src += 3;
    /* strip CR in place */
    char *w = src, *r = src; while (*r) { if (*r != '\r') *w++ = *r; r++; } *w = '\0';

    char *p = src;
    while (p && *p) {
        char *sep = strstr(p, "\n\n");
        if (sep) *sep = '\0';
        process_srt_block(p, out);
        if (!sep) break;
        p = sep + 2;
        while (*p == '\n') p++;   /* collapse runs of blank lines */
    }
    free(copy);
    return out->count > 0;
}

/* ---------- ASS / SSA ---------- */
static void split_ass_fields(const char *s, int count, char fields[][512]) {
    int i = 0; const char *rest = s;
    for (; i < count - 1; i++) {
        const char *comma = strchr(rest, ',');
        if (!comma) { strncpy(fields[i], rest, 511); fields[i][511] = '\0'; rest = ""; continue; }
        size_t n = (size_t)(comma - rest); if (n > 511) n = 511;
        memcpy(fields[i], rest, n); fields[i][n] = '\0';
        rest = comma + 1;
    }
    strncpy(fields[count - 1], rest, 511); fields[count - 1][511] = '\0';
}

static bool parse_ass(const char *text, tb_cue_list *out) {
    char *copy = strdup(text);
    char *line = copy;
    bool in_events = false;
    int fmt_start = -1, fmt_end = -1, fmt_text = -1, fmt_count = 0;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *l = line; while (*l && isspace((unsigned char)*l)) l++;
        size_t ll = strlen(l); while (ll && (l[ll-1] == '\r' || isspace((unsigned char)l[ll-1]))) l[--ll] = '\0';

        if (strncasecmp(l, "[Events]", 8) == 0) { in_events = true; }
        else if (l[0] == '[') { in_events = false; }
        else if (in_events && strncasecmp(l, "Format:", 7) == 0) {
            char *f = l + 7; int idx = 0; char *tok = strtok(f, ",");
            while (tok) { while (*tok == ' ') tok++; if (strcasecmp(tok, "start") == 0) fmt_start = idx; else if (strcasecmp(tok, "end") == 0) fmt_end = idx; else if (strcasecmp(tok, "text") == 0) fmt_text = idx; idx++; tok = strtok(NULL, ","); }
            fmt_count = idx;
        } else if (in_events && strncasecmp(l, "Dialogue:", 9) == 0 && fmt_count > 0 && fmt_start >= 0 && fmt_end >= 0 && fmt_text >= 0) {
            static char fields[32][512];
            if (fmt_count <= 32) {
                split_ass_fields(l + 9, fmt_count, fields);
                double start = tb_parse_subtitle_time(fields[fmt_start]);
                double end = tb_parse_subtitle_time(fields[fmt_end]);
                char *clean = tb_clean_subtitle_text(fields[fmt_text]);
                if (isfinite(start) && isfinite(end) && end > start && clean[0]) tb_cue_list_push(out, start, end, clean);
                free(clean);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(copy);
    return out->count > 0;
}

bool tb_parse_subtitles(const char *text, const char *filename, tb_cue_list *out) {
    bool ok;
    if (ext_is(filename, "ass") || ext_is(filename, "ssa")) ok = parse_ass(text, out);
    else ok = parse_vtt_srt(text, out);
    tb_cue_list_sort(out);
    return ok;
}

bool tb_load_sidecar_subtitles(const char *path, tb_cue_list *out) {
    FILE *f = tb_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f); buf[rd] = '\0';
    fclose(f);
    bool ok = tb_parse_subtitles(buf, path, out);
    if (ok) { strncpy(out->label, path, sizeof out->label - 1); out->label[sizeof out->label - 1] = '\0'; }
    free(buf);
    return ok;
}

/* ---------- sibling file discovery ---------- */
static bool file_exists(const char *p) { FILE *f = tb_fopen(p, "rb"); if (f) { fclose(f); return true; } return false; }

static void replace_ext(const char *path, const char *newext, char *out, size_t outsz) {
    const char *dot = strrchr(path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    snprintf(out, outsz, "%.*s.%s", (int)stem, path, newext);
}

bool tb_find_sidecar_subtitle(const char *media_path, char *out_path, size_t outsz) {
    static const char *exts[] = {"srt", "vtt", "ass", "ssa"};
    for (int i = 0; i < 4; i++) {
        replace_ext(media_path, exts[i], out_path, outsz);
        if (file_exists(out_path)) return true;
    }
    out_path[0] = '\0';
    return false;
}

bool tb_find_cover_art(const char *media_path, char *out_path, size_t outsz) {
    static const char *exts[] = {"png", "jpg", "jpeg"};
    for (int i = 0; i < 3; i++) {
        replace_ext(media_path, exts[i], out_path, outsz);
        if (file_exists(out_path)) return true;
    }
    out_path[0] = '\0';
    return false;
}
