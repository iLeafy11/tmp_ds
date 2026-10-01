#include "snapshot.h"   /* first: ctx.h sets the feature macros */
#include "path.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Persisted snapshot (mmap-based binary format)
 *
 * Format "DS10":
 *   [4 bytes]  magic "DS10"
 *   [4 bytes]  uint32_t root_len
 *   [len bytes] the root's absolute path at save time (no null terminator)
 *   [8 bytes]  uint64_t root_dev      the root's identity on disk: device, inode and birth time
 *   [8 bytes]  uint64_t root_ino      (see search_ctx); zero when unknown
 *   [8 bytes]  int64_t  root_btime
 *   [4 bytes]  uint32_t n_nodes
 *   for each live node, in compact pr_id order:
 *     [4 bytes]  uint32_t path_len
 *     [len bytes] path relative to the root ("" for the root itself; no null terminator)
 *     [8 bytes]  double pgrk
 *     [8 bytes]  double opens (decayed count of opens)
 *     [1 byte]   uint8_t is_dir
 *   [4 bytes]  uint32_t n_edges
 *   for each live-to-live edge:
 *     [4 bytes]  uint32_t from (compact pr_id)
 *     [4 bytes]  uint32_t to   (compact pr_id)
 *     [8 bytes]  double weight
 *   [8 bytes]  int64_t last_decay (unix time the edge weights were last decayed)
 *
 * Paths are relative and the file lives outside the root ($XDG_STATE_HOME), so a root that is
 * renamed, copied or moved never carries a snapshot full of paths that now point elsewhere; the
 * snapshot names its root, and a load for a different root is refused unless the root's identity
 * (device, inode, birth time) is the same, which is what a rename leaves behind: main finds such a
 * file by its identity and adopts it. Every relative path is checked on load: no leading '/', no
 * empty, "." or ".." segment, and the joined absolute path must fit.
 *
 * The last accessed file is not persisted: the file opened last before a stop and the one opened
 * first after a start are not a transition (see ACCESS_MAX_GAP_SECS).
 *
 * Edges whose weight fell below PR_EDGE_MIN_WEIGHT are not written: decay prunes at this door, the
 * same way mummy entries vanish here, and never while running.
 *
 * Runtime graph nodes are never renumbered. Saving walks the index in key order, assigns compact
 * IDs in that order and omits dead (deleted) entries and their edges. Loading recreates nodes in
 * the saved order, so graph IDs and index entries retain the same identity across restarts while
 * ghost nodes vanish; and because that order is sorted, every insert during load appends to the
 * last leaf and takes the skiplist's sequential fast path.
 *
 * The token index is not persisted: it is derived from the paths and rebuilt after the nodes are
 * loaded (DS04 stored it, which made the file trust suffixes it could not check; a suffix the
 * basename does not produce could never be removed again and left a dangling pointer). Rebuilding
 * costs one to four milliseconds per thousand entries, depending on name length.
 */
#define DS_MAGIC "DS10"
#define DS_MAGIC_SIZE 4
#define NODE_FIXED_SIZE (sizeof(uint32_t) + sizeof(double) + sizeof(double) + sizeof(uint8_t))
/* After the magic: root_len, root_dev, root_ino, root_btime (the root string itself is variable). */
#define HEADER_FIXED_SIZE (sizeof(uint32_t) + 2 * sizeof(uint64_t) + sizeof(int64_t))
#define EDGE_RECORD_SIZE ((sizeof(uint32_t) << 1) + sizeof(double))

static bool take_bytes(const char **cursor, const char *end,
                       void *out, size_t n)
{
    size_t remaining = (size_t)(end - *cursor);
    if (n > remaining)
        return false;
    if (out)
        memcpy(out, *cursor, n);
    *cursor += n;
    return true;
}

/* Reads a fixed-size field, bounds-checked. */
#define TAKE(cursor, end, v) take_bytes((cursor), (end), &(v), sizeof(v))

/* The path relative to root: "" for the root itself, NULL if path is not under root. */
static const char *relative_to_root(const char *root, const char *path)
{
    size_t rlen = strlen(root);
    if (!strcmp(path, root))
        return path + rlen;
    if (strncmp(path, root, rlen))
        return NULL;
    if (rlen == 1 && root[0] == '/')
        return path + 1;
    return path[rlen] == '/' ? path + rlen + 1 : NULL;
}

/* A relative path as the file may carry it: nothing absolute, no empty, "." or ".." segment. The
 * empty string is the root itself.
 */
static bool relative_ok(const char *rel, size_t len)
{
    if (!len)
        return true;
    if (rel[0] == '/' || memchr(rel, '\0', len))
        return false;
    for (size_t start = 0, i = 0; i <= len; i++) {
        if (i < len && rel[i] != '/')
            continue;
        size_t seg = i - start;
        if (!seg || (seg == 1 && rel[start] == '.') || (seg == 2 && rel[start] == '.' && rel[start + 1] == '.'))
            return false;
        start = i + 1;
    }
    return true;
}

/* Writing needs no bounds check per field: snapshot_save sizes the mapping up front and verifies
 * that the cursor lands exactly on total_size afterwards.
 */
static char *put_bytes(char *cursor, const void *src, size_t n)
{
    memcpy(cursor, src, n);
    return cursor + n;
}

#define PUT(cursor, v) put_bytes((cursor), &(v), sizeof(v))

static bool size_add(size_t *total, size_t add)
{
    if (add > SIZE_MAX - *total)
        return false;
    *total += add;
    return true;
}

/* Walks the index in key order. Produces old_to_new (old pr_id -> compact id, -1 for ghosts) and
 * ordered (compact id -> entry), so that nodes can be written in key order and edges remapped.
 */
static int build_compact_map(search_ctx *ctx, int **map_out,
                             index_entry ***ordered_out,
                             uint32_t *n_nodes_out, size_t *path_bytes_out)
{
    int graph_n = ctx->graph->n;
    int n_index = ctx->index->size;
    int *old_to_new = malloc((size_t)(graph_n > 0 ? graph_n : 1) * sizeof(*old_to_new));
    index_entry **ordered = malloc((size_t)(n_index > 0 ? n_index : 1) * sizeof(*ordered));
    if (!old_to_new || !ordered) {
        free(old_to_new);
        free(ordered);
        return -1;
    }

    for (int old_id = 0; old_id < graph_n; old_id++)
        old_to_new[old_id] = -1;

    uint32_t n_nodes = 0;
    size_t path_bytes = 0;

    for (bsl_iter it = bsl_seek(ctx->index, ""); it.key; bsl_next(&it)) {
        index_entry *entry = it.slot;
        if (entry->dead)
            continue;
        int old_id = entry->pr_id;

        if (old_id < 0 || old_id >= graph_n || old_id >= ctx->pr_entry_cap ||
            ctx->pr_entries[old_id] != entry) {
            fprintf(stderr, "snapshot_save: %s has inconsistent pr_id %d\n",
                    entry->path, old_id);
            goto fail;
        }

        if ((int)n_nodes >= n_index) {
            fprintf(stderr, "snapshot_save: index size mismatch\n");
            goto fail;
        }

        const char *rel = relative_to_root(ctx->root_path, entry->path);
        if (!rel) {
            fprintf(stderr, "snapshot_save: %s is not under %s\n", entry->path, ctx->root_path);
            goto fail;
        }
        size_t len = strlen(rel);
        if (len > UINT32_MAX || !size_add(&path_bytes, len)) {
            fprintf(stderr, "snapshot_save: path data too large\n");
            goto fail;
        }

        old_to_new[old_id] = (int)n_nodes;
        ordered[n_nodes++] = entry;
    }

    *map_out = old_to_new;
    *ordered_out = ordered;
    *n_nodes_out = n_nodes;
    *path_bytes_out = path_bytes;
    return 0;

fail:
    free(old_to_new);
    free(ordered);
    return -1;
}

/* Counts the edges between live nodes; ghost endpoints are skipped, corrupt ones rejected. */
static int count_edges(search_ctx *ctx, const int *old_to_new, uint32_t *n_edges_out)
{
    uint32_t n_edges = 0;
    for (int old_from = 0; old_from < ctx->graph->n; old_from++) {
        if (old_to_new[old_from] < 0)
            continue;

        pr_node *node = pr_get(ctx->graph, old_from);
        for (int j = 0; j < node->out_degree; j++) {
            int old_to = node->out[j].to->id;
            if (old_to < 0 || old_to >= ctx->graph->n) {
                fprintf(stderr, "snapshot_save: invalid edge target %d\n", old_to);
                return -1;
            }
            if (old_to_new[old_to] < 0 || node->out[j].weight < PR_EDGE_MIN_WEIGHT)
                continue;
            if (n_edges == UINT32_MAX) {
                fprintf(stderr, "snapshot_save: too many live edges\n");
                return -1;
            }
            n_edges++;
        }
    }
    *n_edges_out = n_edges;
    return 0;
}

/* The three section writers each return the advanced cursor. */

static char *write_nodes(char *cursor, search_ctx *ctx, index_entry **ordered, uint32_t n_nodes)
{
    cursor = PUT(cursor, n_nodes);

    /* Compact ID order, which is key order. */
    for (uint32_t i = 0; i < n_nodes; i++) {
        index_entry *entry = ordered[i];
        const char *rel = relative_to_root(ctx->root_path, entry->path);   /* checked by build_compact_map */
        uint32_t path_len = (uint32_t)strlen(rel);
        double pgrk = atomic_load(&entry->pgrk);
        double opens = pr_get(ctx->graph, entry->pr_id)->opens;
        uint8_t is_dir = entry->is_dir;

        cursor = PUT(cursor, path_len);
        cursor = put_bytes(cursor, rel, path_len);
        cursor = PUT(cursor, pgrk);
        cursor = PUT(cursor, opens);
        cursor = PUT(cursor, is_dir);
    }
    return cursor;
}

static char *write_edges(char *cursor, search_ctx *ctx, const int *old_to_new,
                         uint32_t n_edges)
{
    cursor = PUT(cursor, n_edges);

    for (int old_from = 0; old_from < ctx->graph->n; old_from++) {
        if (old_to_new[old_from] < 0)
            continue;

        pr_node *node = pr_get(ctx->graph, old_from);
        for (int j = 0; j < node->out_degree; j++) {
            if (old_to_new[node->out[j].to->id] < 0 || node->out[j].weight < PR_EDGE_MIN_WEIGHT)
                continue;

            uint32_t from = (uint32_t)old_to_new[old_from];
            uint32_t to = (uint32_t)old_to_new[node->out[j].to->id];

            cursor = PUT(cursor, from);
            cursor = PUT(cursor, to);
            cursor = PUT(cursor, node->out[j].weight);
        }
    }

    int64_t last_decay = (int64_t)ctx->last_decay;
    return PUT(cursor, last_decay);
}

static int fsync_parent(const char *filepath)
{
    char *copy = strdup(filepath);
    if (!copy)
        return -1;
    char *slash = strrchr(copy, '/');
    const char *dir = ".";
    if (slash) {
        *slash = '\0';
        dir = slash == copy ? "/" : copy;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    free(copy);
    if (dfd == -1)
        return -1;
    int rc = fsync(dfd);
    close(dfd);
    return rc;
}

/* The file's total size, from the counts build_compact_map and count_edges produced: the fixed
 * header, the root, every node record with its path, the edge records and the decay time. Returns
 * false on overflow.
 */
static bool snapshot_size(uint32_t root_len, uint32_t n_nodes, size_t path_bytes, uint32_t n_edges,
                          size_t *size)
{
    size_t total = DS_MAGIC_SIZE + HEADER_FIXED_SIZE + root_len + sizeof(uint32_t);   /* + n_nodes */
    if (!size_add(&total, (size_t)n_nodes * NODE_FIXED_SIZE) ||
        !size_add(&total, path_bytes) ||
        !size_add(&total, sizeof(uint32_t)) ||
        !size_add(&total, (size_t)n_edges * EDGE_RECORD_SIZE) ||
        !size_add(&total, sizeof(int64_t)))
        return false;
    *size = total;
    return true;
}

/* A unique, exclusively created 0600 temp file next to the target, sized and mapped for writing:
 * mkstemp never follows a symlink or truncates a file someone planted under a predictable name,
 * and two instances saving at once do not share a scratch file.
 *
 * Returns the mapping, or MAP_FAILED. *tmp_path and *fd are set as far as it got, so that the
 * caller cleans up the same way on every path.
 */
static char *map_temp_file(const char *filepath, size_t size, char **tmp_path, int *fd)
{
    size_t tmp_len = strlen(filepath) + sizeof(".XXXXXX");
    *tmp_path = malloc(tmp_len);
    if (!*tmp_path)
        return MAP_FAILED;
    snprintf(*tmp_path, tmp_len, "%s.XXXXXX", filepath);

    *fd = mkstemp(*tmp_path);
    if (*fd == -1) {
        perror("snapshot_save: mkstemp");
        return MAP_FAILED;
    }
    if (ftruncate(*fd, (off_t)size) == -1) {
        perror("snapshot_save: ftruncate");
        return MAP_FAILED;
    }
    char *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (map == MAP_FAILED)
        perror("snapshot_save: mmap");
    return map;
}

/* The magic, then the root and its identity: the counterpart of read_header. */
static char *write_header(char *cursor, const search_ctx *ctx, uint32_t root_len)
{
    cursor = put_bytes(cursor, DS_MAGIC, DS_MAGIC_SIZE);
    cursor = PUT(cursor, root_len);
    cursor = put_bytes(cursor, ctx->root_path, root_len);
    cursor = PUT(cursor, ctx->root_dev);
    cursor = PUT(cursor, ctx->root_ino);
    return PUT(cursor, ctx->root_btime);
}

/* Makes the written temp file the snapshot: flushed to disk, renamed over the target, and the
 * directory synced. fsync of the file makes its bytes durable; the new directory entry is only
 * durable once the directory itself is synced. Unmaps and closes in any case; returns 0 or -1.
 */
static int publish_temp_file(char *map, size_t size, int fd, const char *tmp_path, const char *filepath)
{
    int rc = -1;
    if (msync(map, size, MS_SYNC) == -1)
        perror("snapshot_save: msync");
    else if (fsync(fd) == -1)
        perror("snapshot_save: fsync");
    else
        rc = 0;
    munmap(map, size);
    close(fd);
    if (rc)
        return -1;

    if (rename(tmp_path, filepath) == -1) {
        perror("snapshot_save: rename");
        return -1;
    }
    if (fsync_parent(filepath) == -1)
        perror("snapshot_save: fsync directory");
    return 0;
}

int snapshot_save(search_ctx *ctx, const char *filepath)
{
    int result = -1;
    int *old_to_new = NULL;
    index_entry **ordered = NULL;
    char *tmp_path = NULL;
    char *map = MAP_FAILED;
    int fd = -1;
    size_t size = 0;

    /* Both locks, in the documented order: the index for the entries, the graph for the edges,
     * which pr_loop may be adding to under graph_lock while a periodic snapshot runs. Readers of
     * scores (pgrk) are atomic and need neither. The locks are held only while the snapshot is
     * being written into the mapping; the flush to disk runs without them.
     */
    pthread_rwlock_rdlock(&ctx->lock);
    pthread_rwlock_rdlock(&ctx->graph_lock);

    uint32_t n_nodes = 0, n_edges = 0;
    size_t path_bytes = 0;
    uint32_t root_len = (uint32_t)strlen(ctx->root_path);
    if (build_compact_map(ctx, &old_to_new, &ordered, &n_nodes, &path_bytes) == -1 ||
        count_edges(ctx, old_to_new, &n_edges) == -1)
        goto out_unlock;
    if (!snapshot_size(root_len, n_nodes, path_bytes, n_edges, &size)) {
        fprintf(stderr, "snapshot_save: snapshot too large\n");
        goto out_unlock;
    }
    map = map_temp_file(filepath, size, &tmp_path, &fd);
    if (map == MAP_FAILED)
        goto out_unlock;

    char *cursor = write_header(map, ctx, root_len);
    cursor = write_nodes(cursor, ctx, ordered, n_nodes);
    cursor = write_edges(cursor, ctx, old_to_new, n_edges);
    if ((size_t)(cursor - map) != size) {
        fprintf(stderr, "snapshot_save: internal size mismatch\n");
        goto out_unlock;
    }

    /* Everything the index and the graph had to say is in the mapping. What follows is msync,
     * fsync and rename, hundreds of milliseconds on a slow disk, and holding the locks through it
     * would stall every insert and score sync for that long.
     */
    pthread_rwlock_unlock(&ctx->graph_lock);
    pthread_rwlock_unlock(&ctx->lock);

    result = publish_temp_file(map, size, fd, tmp_path, filepath);
    map = MAP_FAILED;   /* publish_temp_file unmapped and closed both */
    fd = -1;
    goto out;

out_unlock:
    pthread_rwlock_unlock(&ctx->graph_lock);
    pthread_rwlock_unlock(&ctx->lock);
out:
    if (map != MAP_FAILED)
        munmap(map, size);
    if (fd >= 0)
        close(fd);
    if (result != 0 && tmp_path)
        unlink(tmp_path);
    free(tmp_path);
    free(old_to_new);
    free(ordered);
    return result;
}

void snapshot_save_if_changed(search_ctx *ctx)
{
    unsigned gen = atomic_load(&ctx->change_gen);
    if (gen == ctx->saved_gen)
        return;
    if (snapshot_save(ctx, ctx->snapshot_path))
        fprintf(stderr, "snapshot: could not save %s\n", ctx->snapshot_path);
    else
        ctx->saved_gen = gen;   /* changes during the save are caught by the next tick */
}

/* What snapshot_load builds before anything in ctx is touched. After a commit it holds the
 * structures it replaced; either way loaded_free releases what it holds.
 */
struct loaded {
    bsl *index;
    pr_graph *graph;
    index_entry **entries;     /* compact id -> entry; becomes ctx->pr_entries */
    token_index *tokens;
    uint32_t n_nodes;
    int64_t last_decay;
};

static void loaded_free(struct loaded *l)
{
    token_index_destroy(l->tokens);
    bsl_destroy(l->index, index_entry_free);
    pr_destroy(l->graph);
    free(l->entries);
}

/* The magic and the root. The snapshot of another root is refused, unless it is this very directory
 * under a former name: same device, inode and birth time. Relative paths are joined to the current
 * root, so a rename is followed without touching a single record.
 */
static bool read_header(const char **cursor, const char *end, const search_ctx *ctx, const char *filepath)
{
    char magic[DS_MAGIC_SIZE];
    if (!take_bytes(cursor, end, magic, sizeof(magic)) || memcmp(magic, DS_MAGIC, DS_MAGIC_SIZE)) {
        /* Older formats are not loaded (DS01 had unstable graph IDs, DS02 lacks is_dir, DS03 and
         * DS04 carried a token section, DS05 has no decay time, DS06 no opens, DS07 stored the last
         * accessed file, DS08 was skipped, DS09 stored absolute paths and no root); the index is
         * rebuilt from a full scan instead.
         */
        return false;
    }

    uint32_t root_len;
    char saved_root[PATH_MAX];
    uint64_t dev, ino;
    int64_t btime;
    if (!TAKE(cursor, end, root_len) || !root_len || root_len >= sizeof(saved_root) ||
        !take_bytes(cursor, end, saved_root, root_len) ||
        !TAKE(cursor, end, dev) || !TAKE(cursor, end, ino) || !TAKE(cursor, end, btime))
        return false;
    saved_root[root_len] = '\0';
    if (strcmp(saved_root, ctx->root_path) &&
        !(ctx->root_btime && dev == ctx->root_dev && ino == ctx->root_ino && btime == ctx->root_btime)) {
        fprintf(stderr, "snapshot_load: %s is the snapshot of %s, not of %s\n", filepath, saved_root,
                ctx->root_path);
        return false;
    }
    return true;
}

/* "<root>/<rel>", or the root itself for the empty rel; the root "/" gets no second slash. rel is
 * not NUL-terminated (it points into the mapping). NULL if the result would not fit in PATH_MAX or
 * on allocation failure.
 */
static char *absolute_path(const char *root, size_t root_len, const char *rel, size_t rel_len)
{
    size_t sep = rel_len && !(root_len == 1 && root[0] == '/') ? 1 : 0;
    if (root_len + sep + rel_len >= PATH_MAX)
        return NULL;
    char *path = malloc(root_len + sep + rel_len + 1);
    if (!path)
        return NULL;
    memcpy(path, root, root_len);
    if (sep)
        path[root_len] = '/';
    memcpy(path + root_len + sep, rel, rel_len);
    path[root_len + sep + rel_len] = '\0';
    return path;
}

/* The node records, in compact id order: an index entry and a graph node each, every field checked.
 * Paths were saved in key order, so each one goes right behind the previous; the fallback keeps a
 * snapshot with unsorted paths loadable, only slower.
 */
static bool read_nodes(const char **cursor, const char *end, const search_ctx *ctx, struct loaded *l)
{
    uint32_t n_nodes;
    if (!TAKE(cursor, end, n_nodes))
        return false;

    /* Even an empty path needs the fixed part of a node record. */
    if ((uint64_t)n_nodes * NODE_FIXED_SIZE > (uint64_t)(end - *cursor) || n_nodes > INT_MAX)
        return false;

    l->index = bsl_create();
    l->graph = pr_create(n_nodes > 1024 ? (int)n_nodes : 1024);
    l->entries = calloc(n_nodes ? (size_t)n_nodes : 1, sizeof(*l->entries));
    if (!l->index || !l->graph || !l->entries)
        return false;

    size_t root_len = strlen(ctx->root_path);
    bsl_iter cur = {0};
    for (uint32_t i = 0; i < n_nodes; i++) {
        uint32_t path_len;
        if (!TAKE(cursor, end, path_len) || path_len >= PATH_MAX)
            return false;

        if ((size_t)(end - *cursor) < (size_t)path_len + sizeof(double) + sizeof(uint8_t) ||
            !relative_ok(*cursor, path_len))
            return false;

        index_entry *entry = calloc(1, sizeof(*entry));
        if (!entry)
            return false;
        entry->path = absolute_path(ctx->root_path, root_len, *cursor, path_len);
        if (!entry->path) {
            free(entry);
            return false;
        }
        *cursor += path_len;

        double pgrk, opens;
        uint8_t is_dir;
        if (!TAKE(cursor, end, pgrk) ||
            !TAKE(cursor, end, opens) ||
            !TAKE(cursor, end, is_dir) ||
            is_dir > 1 || !isfinite(pgrk) || pgrk < 0.0 || !isfinite(opens) || opens < 0.0) {
            index_entry_free(entry);
            return false;
        }
        atomic_store(&entry->pgrk, pgrk);
        entry->is_dir = is_dir;

        entry->seen = true;
        entry->pr_id = pr_add_node(l->graph);
        if (entry->pr_id != (int)i) {
            index_entry_free(entry);
            return false;
        }

        if (bsl_insert_after(l->index, &cur, entry->path, entry)) {
            if (bsl_insert(l->index, entry->path, entry)) {
                index_entry_free(entry);
                return false;
            }
            cur = bsl_seek(l->index, entry->path);
        }

        atomic_store(&pr_get(l->graph, i)->p, pgrk);
        pr_get(l->graph, i)->opens = opens;
        l->entries[i] = entry;
    }
    l->n_nodes = n_nodes;
    return true;
}

/* The edges between live nodes by compact id, every weight checked, then the decay time: the
 * counterpart of write_edges.
 */
static bool read_edges(const char **cursor, const char *end, struct loaded *l)
{
    uint32_t n_edges;
    if (!TAKE(cursor, end, n_edges) || (uint64_t)n_edges * EDGE_RECORD_SIZE > (uint64_t)(end - *cursor))
        return false;

    for (uint32_t i = 0; i < n_edges; i++) {
        uint32_t from, to;
        double weight;

        if (!TAKE(cursor, end, from) ||
            !TAKE(cursor, end, to) ||
            !TAKE(cursor, end, weight) ||
            from >= l->n_nodes || to >= l->n_nodes || !isfinite(weight) || weight < 0.0 ||
            pr_add_edge(pr_get(l->graph, (int)from), pr_get(l->graph, (int)to), weight))
            return false;
    }

    return TAKE(cursor, end, l->last_decay) && l->last_decay >= 0;
}

/* Swaps the loaded structures into ctx under the index lock; l is left holding the ones they
 * replaced, for loaded_free outside the lock. Called only once the whole file has been validated
 * and built.
 */
static void commit_loaded(search_ctx *ctx, struct loaded *l)
{
    pthread_rwlock_wrlock(&ctx->lock);
    bsl *index = ctx->index;
    pr_graph *graph = ctx->graph;
    index_entry **entries = ctx->pr_entries;
    token_index *tokens = ctx->tokens;

    ctx->index = l->index;
    ctx->graph = l->graph;
    ctx->pr_entries = l->entries;
    ctx->pr_entry_cap = (int)l->n_nodes;
    ctx->tokens = l->tokens;
    ctx->n_live = (int)l->n_nodes;
    ctx->last_decay = (time_t)l->last_decay;
    atomic_fetch_add(&ctx->index_gen, 1);

    l->index = index;
    l->graph = graph;
    l->entries = entries;
    l->tokens = tokens;
    pthread_rwlock_unlock(&ctx->lock);
}

int snapshot_load(search_ctx *ctx, const char *filepath)
{
    int fd = open(filepath, O_RDONLY);
    if (fd == -1)
        return -1;

    int result = -1;
    char *map = MAP_FAILED;
    struct loaded l = {0};

    struct stat st;
    if (fstat(fd, &st) == -1 || st.st_size <
        (off_t)(DS_MAGIC_SIZE + HEADER_FIXED_SIZE + 2 * sizeof(uint32_t) + sizeof(int64_t)))
        goto out;

    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        perror("snapshot_load: mmap");
        goto out;
    }

    const char *cursor = map;
    const char *end = map + st.st_size;
    if (!read_header(&cursor, end, ctx, filepath) || !read_nodes(&cursor, end, ctx, &l) ||
        !read_edges(&cursor, end, &l) || cursor != end)
        goto out;

    /* Derived data: the token index is rebuilt from the loaded paths, on the private structures
     * before the swap, so nothing the file says about suffixes is ever trusted.
     */
    l.tokens = token_index_create();
    if (!l.tokens)
        goto out;
    for (uint32_t i = 0; i < l.n_nodes; i++)
        token_index_add(l.tokens, l.entries[i]->path, l.entries[i]);

    commit_loaded(ctx, &l);   /* l now holds what was replaced; freed below */

    /* The time the daemon was not running counts: decay the loaded weights for it now. A file from
     * before any decay (last_decay 0) is left as it is and dated from here.
     */
    if (ctx->last_decay == 0)
        ctx->last_decay = time(NULL);
    else
        ctx_decay(ctx, time(NULL));

    result = 0;

out:
    loaded_free(&l);
    if (map != MAP_FAILED)
        munmap(map, (size_t)st.st_size);
    close(fd);
    return result;
}

/* Reads only the header of a snapshot: the root it was saved for and that root's identity.
 * Returns 0, or -1 if the file is not a snapshot of the current format.
 */
static int peek_header(const char *filepath, char *root, size_t size, uint64_t *dev, uint64_t *ino,
                       int64_t *btime)
{
    int fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
        return -1;

    int rc = -1;
    char magic[DS_MAGIC_SIZE];
    uint32_t root_len;
    if (read(fd, magic, sizeof(magic)) != (ssize_t)sizeof(magic) || memcmp(magic, DS_MAGIC, DS_MAGIC_SIZE) ||
        read(fd, &root_len, sizeof(root_len)) != (ssize_t)sizeof(root_len) ||
        !root_len || root_len >= size ||
        read(fd, root, root_len) != (ssize_t)root_len ||
        read(fd, dev, sizeof(*dev)) != (ssize_t)sizeof(*dev) ||
        read(fd, ino, sizeof(*ino)) != (ssize_t)sizeof(*ino) ||
        read(fd, btime, sizeof(*btime)) != (ssize_t)sizeof(*btime))
        goto out;
    root[root_len] = '\0';
    rc = 0;
out:
    close(fd);
    return rc;
}

/* Moves one file, by rename or, across file systems, by copy and unlink. The copy goes to a new
 * name only (O_EXCL), so a file that appeared at the destination meanwhile is never overwritten.
 */
static void move_file(const char *from, const char *to)
{
    if (!rename(from, to) || errno != EXDEV)
        return;
    int in = open(from, O_RDONLY | O_CLOEXEC);
    int out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = out >= 0;
    char buf[65536];
    for (ssize_t n; ok && (n = read(in, buf, sizeof(buf))) != 0;)
        ok = n > 0 && write(out, buf, (size_t)n) == n;
    ok = ok && !fsync(out);
    if (out >= 0)
        close(out);
    if (in >= 0)
        close(in);
    if (ok)
        unlink(from);
    else if (out >= 0)
        unlink(to);
}

/* Snapshots used to live in $XDG_CACHE_HOME/desktop-search. They carry the access history, which
 * nothing can rebuild, while a cache is by definition something that may be deleted; the XDG spec
 * puts data like this under $XDG_STATE_HOME. Whatever is still at the old place moves over the
 * first time the directory is asked for, and the old directory goes once it is empty. A file
 * already at the new place is newer and stays; its old copy is left where it is.
 */
static void move_old_snapshots(const char *dir)
{
    char old[PATH_MAX];
    if (!path_xdg_path("XDG_CACHE_HOME", ".cache", old, sizeof(old)))
        return;
    DIR *d = opendir(old);
    if (!d)
        return;
    for (struct dirent *e; (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 4, ".bin"))
            continue;
        char from[PATH_MAX], to[PATH_MAX];
        if (path_join(from, sizeof(from), old, e->d_name) < 0 || path_join(to, sizeof(to), dir, e->d_name) < 0 ||
            !access(to, F_OK))
            continue;
        move_file(from, to);
    }
    closedir(d);
    rmdir(old);   /* only if empty */
}

bool snapshot_dir(char *dir, size_t size)
{
    char base[PATH_MAX];
    if (!path_xdg_dir("XDG_STATE_HOME", ".local/state", base, sizeof(base)) ||
        path_join(dir, size, base, "snapshots") < 0 || (mkdir(dir, 0700) && errno != EEXIST))
        return false;
    move_old_snapshots(dir);
    return true;
}

/* A root that was renamed has its snapshot under the hash of its old name. The file names its
 * root's identity (device, inode, birth time), which the rename kept, so the directory is searched
 * for it and the file takes the new name; its relative paths then load under the new root without a
 * rescan and the history survives. Nothing found means a fresh start.
 *
 * Two cases look like a rename and are not. With no birth time the identity is an inode number,
 * which a deleted directory hands on to a new one, so nothing is adopted. And the old name may
 * still lead to the very same directory (a bind mount, or the same tree mounted twice): that is a
 * second path, not a former one, and taking its snapshot would only have the two paths take it
 * back and forth.
 */
static void adopt_renamed_snapshot(search_ctx *ctx, const char *dir)
{
    if (!access(ctx->snapshot_path, F_OK) || !ctx->root_btime)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    for (struct dirent *e; (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 4, ".bin"))
            continue;
        char file[PATH_MAX], saved_root[PATH_MAX];
        uint64_t dev, ino;
        int64_t btime;
        if (path_join(file, sizeof(file), dir, e->d_name) < 0 ||
            peek_header(file, saved_root, sizeof(saved_root), &dev, &ino, &btime) ||
            dev != ctx->root_dev || ino != ctx->root_ino || btime != ctx->root_btime)
            continue;
        uint64_t old_dev, old_ino;
        int64_t old_btime;
        if (path_identity(saved_root, &old_dev, &old_ino, &old_btime) && old_dev == dev && old_ino == ino &&
            old_btime == btime)
            continue;   /* the old name still reaches it: a second path, not a rename */
        if (!rename(file, ctx->snapshot_path))
            fprintf(stderr, "adopted the snapshot of %s: same directory, renamed to %s\n", saved_root,
                    ctx->root_path);
        break;
    }
    closedir(d);
}

/* The file is named by a hash of the root path rather than the path itself: one file per root,
 * a name that needs no escaping, outside the root (so it never travels with a copy or a rename,
 * and a read-only root can be indexed). FNV-1a is enough: the name only needs to separate roots,
 * not resist anyone.
 */
int snapshot_locate(search_ctx *ctx)
{
    char dir[PATH_MAX];
    if (!snapshot_dir(dir, sizeof(dir)))
        return -1;
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)ctx->root_path; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    char name[32];
    snprintf(name, sizeof(name), "%016llx.bin", (unsigned long long)h);
    if (path_join(ctx->snapshot_path, sizeof(ctx->snapshot_path), dir, name) < 0)
        return -1;
    adopt_renamed_snapshot(ctx, dir);
    return 0;
}
