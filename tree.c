// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include "object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

// Recursive helper: build a tree for a "slice" of index entries that all share
// the same directory prefix at the given depth level.
//
// entries : pointer into the index entries array (already sorted by path)
// count   : number of entries in this slice
// prefix  : the directory prefix that all entries in this slice share,
//           e.g. "src/" at depth 1.  Empty string ("") at the root level.
// id_out  : receives the ObjectID of the written tree object
//
// Returns 0 on success, -1 on error.
static int write_tree_level(const IndexEntry *entries, int count,
                            const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Each entry's path relative to this level is:
        //   full_path + strlen(prefix)
        const char *rel = entries[i].path + strlen(prefix);

        // Does a '/' appear in the remainder?  If so, this entry lives in a
        // subdirectory at this level.
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // ── Plain file entry ─────────────────────────────────────────
            // rel is just "filename" with no further nesting.
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;
        } else {
            // ── Subdirectory entry ───────────────────────────────────────
            // Extract the immediate child directory name, e.g. "src" from
            // "src/main.c".
            size_t dir_name_len = slash - rel;
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build the new prefix for the recursive call, e.g. "src/"
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);
            size_t sub_prefix_len = strlen(sub_prefix);

            // Collect all entries that start with sub_prefix (i.e., belong to
            // this subdirectory).
            int j = i;
            while (j < count &&
                   strncmp(entries[j].path, sub_prefix, sub_prefix_len) == 0) {
                j++;
            }
            // entries[i..j-1] all live under sub_prefix.

            // Recurse to build the subtree.
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, sub_prefix, &sub_id) != 0)
                return -1;

            // Add a directory entry pointing at the subtree.
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = sub_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';

            i = j; // skip past all entries we just handled
        }
    }

    // Serialize and write this tree level to the object store.
    void   *data = NULL;
    size_t  data_len = 0;
    if (tree_serialize(&tree, &data, &data_len) != 0) return -1;

    int ret = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return ret;
}

// Comparison function for sorting IndexEntry pointers by path (for qsort).
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

// Build a tree hierarchy from the current index and write all tree objects to
// the object store.  Returns the root tree's ObjectID in *id_out.
int tree_from_index(ObjectID *id_out) {
    Index *index = malloc(sizeof(Index));
    if (!index) return -1;
    if (index_load(index) != 0) {
        free(index);
        return -1;
    }
    if (index->count == 0) {
        free(index);
        return -1; // nothing staged
    }

    // Sort entries by path so that sibling files and directories are adjacent.
    // write_tree_level relies on this ordering to group subdirectory entries.
    qsort(index->entries, index->count, sizeof(IndexEntry), compare_index_entries);

    // Kick off the recursive build from the root (empty prefix).
    int rc = write_tree_level(index->entries, index->count, "", id_out);
    free(index);
    return rc;
}