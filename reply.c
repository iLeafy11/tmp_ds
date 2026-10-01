#include "reply.h"   /* first: ctx.h sets the feature macros */
#include "utf8.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Writing */

/* JSON string: quotes, backslashes and control characters escaped. Indexed names are well-formed
 * UTF-8 by construction (scan filters them), so the U+FFFD fallback is a guard for anything else
 * that reaches here, keeping the output valid JSON whatever the input.
 */
static void write_json_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            fprintf(out, "\\%c", c);
            p++;
        } else if (c < 0x20) {
            fprintf(out, "\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            fputc(c, out);
            p++;
        } else {
            int n = utf8_seq_len((const char *)p);
            if (n) {
                fwrite(p, 1, (size_t)n, out);
                p += n;
            } else {
                fputs("\xef\xbf\xbd", out);
                p++;
            }
        }
    }
    fputc('"', out);
}

void reply_fill_status(search_ctx *ctx, query_reply *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->root, sizeof(r->root), "%s", ctx->root_path);
    ctx_get_status(ctx, &r->status);
}

void reply_fill(search_ctx *ctx, const char *text, query_reply *r)
{
    reply_fill_status(ctx, r);
    r->total = search_query(ctx, text, r->hits, &r->nhits);
}

void reply_write(FILE *out, const query_reply *r)
{
    fprintf(out, "{\"total\":%d,\"root\":", r->total);
    write_json_string(out, r->root);
    fprintf(out, ",\"indexed\":%d,\"scan_done\":%s,\"unwatched\":%d,\"root_lost\":%s,"
                 "\"index_gen\":%u,\"score_gen\":%u,\"hits\":[",
            r->status.indexed, r->status.scan_done ? "true" : "false", r->status.unwatched,
            r->status.root_lost ? "true" : "false", r->status.index_gen, r->status.score_gen);
    for (int i = 0; i < r->nhits; i++) {
        fprintf(out, "%s{\"path\":", i ? "," : "");
        write_json_string(out, r->hits[i].path);
        fprintf(out, ",\"score\":%.4f,\"is_dir\":%s}", r->hits[i].score,
                r->hits[i].is_dir ? "true" : "false");
    }
    fputs("]}\n", out);
}

/* Reading */

/* A parser for exactly the shape reply_write produces: one object whose members are numbers,
 * booleans, one string and one array of flat objects. It tolerates whitespace and any member order,
 * and rejects everything else. A general JSON library would be ten times the code for a format that
 * has one producer.
 */
typedef struct parser {
    const char *p;
} parser;

static void skip_ws(parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static bool expect(parser *ps, char c)
{
    skip_ws(ps);
    if (*ps->p != c)
        return false;
    ps->p++;
    return true;
}

static void put_utf8(char *out, size_t size, size_t *n, unsigned cp)
{
    char tmp[4];
    int len;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        len = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    }
    if (*n + (size_t)len < size) {
        memcpy(out + *n, tmp, (size_t)len);
        *n += (size_t)len;
    }
}

/* A JSON string into out (NUL-terminated, truncated to size). Escapes are decoded; \u only for the
 * Basic Multilingual Plane, which is all the server ever emits (control characters).
 */
static bool parse_string(parser *ps, char *out, size_t size)
{
    if (!expect(ps, '"'))
        return false;
    size_t n = 0;
    for (;;) {
        char c = *ps->p++;
        if (c == '\0')
            return false;
        if (c == '"')
            break;
        if (c != '\\') {
            if (n + 1 < size)
                out[n++] = c;
            continue;
        }
        c = *ps->p++;
        switch (c) {
        case '"': case '\\': case '/':
            if (n + 1 < size)
                out[n++] = c;
            break;
        case 'b': if (n + 1 < size) out[n++] = '\b'; break;
        case 'f': if (n + 1 < size) out[n++] = '\f'; break;
        case 'n': if (n + 1 < size) out[n++] = '\n'; break;
        case 'r': if (n + 1 < size) out[n++] = '\r'; break;
        case 't': if (n + 1 < size) out[n++] = '\t'; break;
        case 'u': {
            char hex[5] = {0};
            for (int i = 0; i < 4; i++) {
                char h = ps->p[i];
                if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')))
                    return false;
                hex[i] = h;
            }
            ps->p += 4;
            unsigned cp = (unsigned)strtoul(hex, NULL, 16);
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0xFFFD;   /* surrogates never appear in our output; replace rather than trust */
            put_utf8(out, size, &n, cp);
            break;
        }
        default:
            return false;
        }
    }
    out[n] = '\0';
    return true;
}

static bool parse_number(parser *ps, double *out)
{
    skip_ws(ps);
    char *end;
    *out = strtod(ps->p, &end);
    if (end == ps->p)
        return false;
    ps->p = end;
    return true;
}

static bool parse_bool(parser *ps, bool *out)
{
    skip_ws(ps);
    if (!strncmp(ps->p, "true", 4)) {
        ps->p += 4;
        *out = true;
        return true;
    }
    if (!strncmp(ps->p, "false", 5)) {
        ps->p += 5;
        *out = false;
        return true;
    }
    return false;
}

static bool parse_hit(parser *ps, search_hit *hit)
{
    if (!expect(ps, '{'))
        return false;
    memset(hit, 0, sizeof(*hit));
    for (;;) {
        char key[16];
        if (!parse_string(ps, key, sizeof(key)) || !expect(ps, ':'))
            return false;
        double d;
        if (!strcmp(key, "path")) {
            if (!parse_string(ps, hit->path, sizeof(hit->path)))
                return false;
        } else if (!strcmp(key, "score")) {
            if (!parse_number(ps, &d) || !isfinite(d))
                return false;
            hit->score = d;
        } else if (!strcmp(key, "is_dir")) {
            if (!parse_bool(ps, &hit->is_dir))
                return false;
        } else {
            return false;
        }
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        return expect(ps, '}');
    }
}

int reply_parse(const char *json, query_reply *out)
{
    parser ps = {.p = json};
    memset(out, 0, sizeof(*out));
    if (!expect(&ps, '{'))
        return -1;
    skip_ws(&ps);
    if (*ps.p == '}')
        return -1;   /* the reply is never empty */
    for (;;) {
        char key[16];
        if (!parse_string(&ps, key, sizeof(key)) || !expect(&ps, ':'))
            return -1;
        double d;
        if (!strcmp(key, "total")) {
            if (!parse_number(&ps, &d)) return -1;
            out->total = (int)d;
        } else if (!strcmp(key, "indexed")) {
            if (!parse_number(&ps, &d)) return -1;
            out->status.indexed = (int)d;
        } else if (!strcmp(key, "unwatched")) {
            if (!parse_number(&ps, &d)) return -1;
            out->status.unwatched = (int)d;
        } else if (!strcmp(key, "index_gen")) {
            if (!parse_number(&ps, &d)) return -1;
            out->status.index_gen = (unsigned)d;
        } else if (!strcmp(key, "score_gen")) {
            if (!parse_number(&ps, &d)) return -1;
            out->status.score_gen = (unsigned)d;
        } else if (!strcmp(key, "scan_done")) {
            if (!parse_bool(&ps, &out->status.scan_done)) return -1;
        } else if (!strcmp(key, "root_lost")) {
            if (!parse_bool(&ps, &out->status.root_lost)) return -1;
        } else if (!strcmp(key, "root")) {
            if (!parse_string(&ps, out->root, sizeof(out->root))) return -1;
        } else if (!strcmp(key, "hits")) {
            if (!expect(&ps, '['))
                return -1;
            skip_ws(&ps);
            if (*ps.p == ']') {
                ps.p++;
            } else {
                for (;;) {
                    if (out->nhits >= SEARCH_TOP_K)
                        return -1;
                    if (!parse_hit(&ps, &out->hits[out->nhits]))
                        return -1;
                    out->nhits++;
                    skip_ws(&ps);
                    if (*ps.p == ',') {
                        ps.p++;
                        continue;
                    }
                    if (!expect(&ps, ']'))
                        return -1;
                    break;
                }
            }
        } else {
            return -1;
        }
        skip_ws(&ps);
        if (*ps.p == ',') {
            ps.p++;
            continue;
        }
        if (!expect(&ps, '}'))
            return -1;
        skip_ws(&ps);
        return *ps.p == '\0' ? 0 : -1;
    }
}
