#include "tui.h"   /* first: ctx.h sets the feature macros before termbox2 sets its own */

#define TB_IMPL
#include "termbox2.h"

#include <stdio.h>
#include <string.h>

#define MAX_QUERY 256
#define COLOR_DIR 208
#define COLOR_REG 117
#define COLOR_PROMPT 255
#define COLOR_MATCH 226

/* Key polling interval; also how often, while idle, the current query is re-run to notice that the
 * index or the scores changed underneath the results. Over the socket that is one small round trip
 * per tick, which is nothing.
 */
#define REFRESH_MS 250

typedef struct tui_state {
    const tui_backend *be;
    char query[MAX_QUERY];
    int qlen;
    int cpos;
    query_reply r;          /* the last reply: hits plus the status shown in the label */
    bool backend_gone;      /* last query failed; the reply shown is stale */
    int root_prefix_len;    /* strlen(r.root) + 1 unless root is "/", for root-relative display */
} tui_state;

static void draw_separator(int y, int w, const char *label)
{
    for (int x = 0; x < w; x++)
        tb_set_cell(x, y, 0x2500, TB_WHITE, TB_DEFAULT);

    if (label) {
        int llen = (int)strlen(label);
        int start = w - llen - 3;
        if (start > 0) {
            tb_set_cell(start, y, ' ', TB_DEFAULT, TB_DEFAULT);
            for (int i = 0; i < llen; i++)
                tb_set_cell(start + 1 + i, y, (uint32_t)label[i],
                            TB_WHITE | TB_BOLD, TB_DEFAULT);
            tb_set_cell(start + 1 + llen, y, ' ', TB_DEFAULT, TB_DEFAULT);
        }
    }
}

/* UTF-8 support is intentionally small: termbox2 already gives key events as Unicode code points,
 * so the TUI only needs byte encoding, code-point-aware backspace, and display-width accounting. No
 * normalization is performed.
 */
#define ELLIPSIS "\xe2\x80\xa6"   /* U+2026, one column */

static int utf8_display_width(const char *s);

/* Fits a root-relative path into max_width columns by dropping directories from the middle,
 * keeping what a person recognises a file by: the first directory, the one it is in, and its name:
 * "root/.../XXX/YYY.mp3". Progressively "…/XXX/YYY.mp3", "…/YYY.mp3", and as a last resort the name
 * cut short with "…". out always holds something printable.
 */
static void abbreviate_path(const char *path, int max_width, char *out, size_t out_size)
{
    if (utf8_display_width(path) <= max_width) {
        snprintf(out, out_size, "%s", path);
        return;
    }

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    /* The directory holding the file: the component right before the name. */
    const char *last_dir = NULL;
    int last_dir_len = 0;
    if (name > path) {
        const char *end = name - 1;              /* the slash before the name */
        const char *start = end;
        while (start > path && start[-1] != '/')
            start--;
        last_dir = start;
        last_dir_len = (int)(end - start);
    }

    const char *first_end = strchr(path, '/');
    int first_len = first_end ? (int)(first_end - path) : 0;

    if (last_dir && last_dir > path + first_len + 1) {
        snprintf(out, out_size, "%.*s/" ELLIPSIS "/%.*s/%s", first_len, path, last_dir_len,
                 last_dir, name);
        if (utf8_display_width(out) <= max_width)
            return;
    }
    if (last_dir) {
        snprintf(out, out_size, ELLIPSIS "/%.*s/%s", last_dir_len, last_dir, name);
        if (utf8_display_width(out) <= max_width)
            return;
        snprintf(out, out_size, ELLIPSIS "/%s", name);
        if (utf8_display_width(out) <= max_width)
            return;
    }

    /* Even the name alone is too wide: keep as many leading columns as fit, then the ellipsis. */
    int width = 0;
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < out_size; ) {
        uint32_t ch;
        int len = tb_utf8_char_to_unicode(&ch, p);
        if (len <= 0)
            len = 1;
        int w = len == 1 && (unsigned char)*p < 0x80 ? 1 : tb_wcwidth(ch);
        if (w <= 0)
            w = 1;
        if (width + w > max_width - 1 || n + (size_t)len >= out_size)
            break;
        memcpy(out + n, p, (size_t)len);
        n += (size_t)len;
        width += w;
        p += len;
    }
    snprintf(out + n, out_size - n, "%s", ELLIPSIS);
}

static int utf8_display_width(const char *s)
{
    int width = 0;
    while (*s) {
        uint32_t ch;
        int n = tb_utf8_char_to_unicode(&ch, s);
        if (n <= 0) {
            s++;
            width++;
            continue;
        }
        int w = tb_wcwidth(ch);
        width += w > 0 ? w : 1;
        s += n;
    }
    return width;
}

static int utf8_prev(const char *s, int pos)
{
    if (pos <= 0)
        return 0;
    int i = pos - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        i--;
    return i;
}

static int utf8_next(const char *s, int pos, int len)
{
    if (pos >= len)
        return len;
    int i = pos + 1;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
        i++;
    return i;
}

static void utf8_delete_at(char *s, int *len, int *cpos)
{
    if (*cpos <= 0)
        return;
    int prev = utf8_prev(s, *cpos);
    int tail = *len - *cpos;
    memmove(s + prev, s + *cpos, tail + 1);
    *len -= (*cpos - prev);
    *cpos = prev;
}

static bool utf8_insert_at(char *s, int *len, int *cpos, int cap, uint32_t ch)
{
    char encoded[8];
    int n = tb_utf8_unicode_to_char(encoded, ch);
    if (n <= 0 || *len + n >= cap)
        return false;
    int tail = *len - *cpos;
    memmove(s + *cpos + n, s + *cpos, tail + 1);
    memcpy(s + *cpos, encoded, (size_t)n);
    *len += n;
    *cpos += n;
    return true;
}

static void draw(const tui_state *s)
{
    tb_clear();

    int w = tb_width();
    int h = tb_height();
    if (h < 3 || w < 10) {
        tb_present();
        return;
    }

    draw_separator(h - 1, w, NULL);

    int input_y = h - 2;
    tb_set_cell(0, input_y, 0x276F, COLOR_PROMPT | TB_BOLD, TB_DEFAULT);
    tb_print(2, input_y, COLOR_PROMPT, TB_DEFAULT, s->query);

    int query_width = utf8_display_width(s->query);
    if (s->qlen > 0) {
        char info[64];
        snprintf(info, sizeof(info), "[%d/%d]", s->r.nhits, s->r.total);
        int ix = w - (int)strlen(info);
        if (ix > query_width + 4)
            tb_print(ix, input_y, COLOR_MATCH, TB_DEFAULT, info);
    }

    char tmp[MAX_QUERY];
    memcpy(tmp, s->query, s->cpos);
    tmp[s->cpos] = '\0';
    int cursor_width = utf8_display_width(tmp);
    tb_set_cursor(2 + cursor_width, input_y);

    if (h >= 4) {
        char label[128], unwatched[32] = "";
        if (s->r.status.unwatched)
            snprintf(unwatched, sizeof(unwatched), ", %d unwatched", s->r.status.unwatched);
        snprintf(label, sizeof(label), "Desktop Search  %d indexed%s%s%s%s",
                 s->r.status.indexed, s->r.status.scan_done ? "" : ", scanning", unwatched,
                 s->r.status.root_lost ? ", ROOT GONE" : "",
                 s->backend_gone ? ", DAEMON GONE" : "");
        draw_separator(h - 3, w, label);
    }

    int result_lines = h - 3;
    if (result_lines > 0) {
        int lines = s->r.nhits < result_lines ? s->r.nhits : result_lines;
        int root_prefix_len = s->root_prefix_len;
        for (int i = 0; i < lines; i++) {
            const search_hit *hit = &s->r.hits[i];
            const char *path = hit->path;
            if (root_prefix_len > 0 &&
                (int)strlen(path) > root_prefix_len)
                path += root_prefix_len;

            /* The score sits at the right edge, 0.0000 included so the column is always there; the
             * path gets what is left, minus a gap. Widths are display columns (CJK characters take
             * two), never byte counts.
             */
            char sbuf[32];
            snprintf(sbuf, sizeof(sbuf), "%.4f", hit->score);
            int score_w = (int)strlen(sbuf) + 1;
            int max_path_w = w - 2 - score_w - 2;
            if (max_path_w < 4)
                max_path_w = 4;

            char shown[PATH_MAX];
            abbreviate_path(path, max_path_w, shown, sizeof(shown));

            size_t out_w = 0;
            const char *slash = hit->is_dir ? NULL : strrchr(shown, '/');
            if (slash) {
                tb_printf_ex(2, i, COLOR_DIR, TB_DEFAULT, &out_w, "%.*s",
                             (int)(slash - shown + 1), shown);
                tb_print(2 + (int)out_w, i, COLOR_REG, TB_DEFAULT, slash + 1);
            } else {
                tb_print(2, i, hit->is_dir ? COLOR_DIR : COLOR_REG, TB_DEFAULT, shown);
            }

            tb_print(w - score_w, i, TB_CYAN, TB_DEFAULT, sbuf);
        }
    }

    tb_present();
}

/* Runs the current query against the backend and takes the reply. The caller always redraws: after
 * a keystroke the screen must follow the query, and on failure the label flips to say the backend
 * is gone while the previous reply stays on screen.
 */
static void run_query(tui_state *s)
{
    query_reply fresh;
    if (s->be->query(s->be->arg, s->query, &fresh)) {
        s->backend_gone = true;
        return;
    }
    s->backend_gone = false;
    s->r = fresh;
    size_t rlen = strlen(s->r.root);
    s->root_prefix_len = rlen && s->r.root[rlen - 1] == '/' ? (int)rlen : (int)rlen + 1;
}

static bool status_changed(const ctx_status *a, const ctx_status *b)
{
    return a->index_gen != b->index_gen || a->score_gen != b->score_gen ||
           a->scan_done != b->scan_done || a->unwatched != b->unwatched ||
           a->root_lost != b->root_lost;
}

/* Idle tick: asks the backend for its status alone, and reruns the query only when the index or
 * the scores moved, or the backend came or went. Returns whether the screen needs a redraw.
 */
static bool refresh_query(tui_state *s)
{
    query_reply st;
    if (s->be->status(s->be->arg, &st)) {
        bool changed = !s->backend_gone;
        s->backend_gone = true;
        return changed;
    }
    if (!s->backend_gone && !status_changed(&st.status, &s->r.status))
        return false;
    run_query(s);
    return true;
}

void *tui_loop(void *arg)
{
    if (tb_init() != TB_OK) {
        fprintf(stderr, "tb_init failed\n");
        return NULL;
    }
    tb_set_output_mode(TB_OUTPUT_256);

    tui_state *s = calloc(1, sizeof(*s));
    if (!s) {
        tb_shutdown();
        return NULL;
    }
    s->be = arg;

    run_query(s);
    draw(s);

    for (;;) {
        /* Set by event_loop on SIGINT/SIGTERM/SIGHUP; checked every tick so the terminal is
         * restored and main's normal shutdown (including the save) runs.
         */
        if (s->be->should_quit && s->be->should_quit(s->be->arg))
            break;

        struct tb_event ev;
        int rc = tb_peek_event(&ev, REFRESH_MS);
        if (rc == TB_ERR_NO_EVENT) {
            if (refresh_query(s))
                draw(s);
            continue;
        }
        if (rc != TB_OK)
            break;

        if (ev.type == TB_EVENT_RESIZE) {
            draw(s);
            continue;
        }
        if (ev.type != TB_EVENT_KEY)
            continue;

        /* Raw mode turns Ctrl+C into a key event rather than SIGINT, so it is handled here. */
        if (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C)
            break;

        if (ev.key == TB_KEY_ENTER) {
            if (!strcmp(s->query, "quit") || !strcmp(s->query, "exit"))
                break;
            continue;
        }

        if (ev.key == TB_KEY_ARROW_LEFT) {
            s->cpos = utf8_prev(s->query, s->cpos);
            draw(s);
            continue;
        }
        if (ev.key == TB_KEY_ARROW_RIGHT) {
            s->cpos = utf8_next(s->query, s->cpos, s->qlen);
            draw(s);
            continue;
        }

        if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
            utf8_delete_at(s->query, &s->qlen, &s->cpos);
        } else if (ev.ch) {
            utf8_insert_at(s->query, &s->qlen, &s->cpos, MAX_QUERY, ev.ch);
        } else {
            continue;
        }

        run_query(s);
        draw(s);
    }

    tb_shutdown();
    free(s);
    return TUI_OK;
}
