// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char type_str[10];

    if (type == OBJ_BLOB) strcpy(type_str, "blob");
    else if (type == OBJ_TREE) strcpy(type_str, "tree");
    else if (type == OBJ_COMMIT) strcpy(type_str, "commit");
    else return -1;

    // 1. Header
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    // 2. Combine header + data
    size_t total_size = header_len + len;
    unsigned char *buffer = malloc(total_size);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);
    memcpy(buffer + header_len, data, len);

    // 3. Compute hash
    compute_hash(buffer, total_size, id_out);

    // 4. Deduplication
    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    // 5. Get final path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // Extract directory path
    char dir[512];
    strcpy(dir, path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        free(buffer);
        return -1;
    }
    *slash = '\0';

    mkdir(dir, 0755);

    // 6. Temp file
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(buffer);
        return -1;
    }

    // 7. Write data
    if (write(fd, buffer, total_size) != (ssize_t)total_size) {
        close(fd);
        free(buffer);
        return -1;
    }

    // 8. fsync
    fsync(fd);
    close(fd);

    // 9. Atomic rename
    if (rename(temp_path, path) != 0) {
        free(buffer);
        return -1;
    }

    free(buffer);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);

    unsigned char *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    fread(buffer, 1, size, f);
    fclose(f);

    // Verify integrity
    ObjectID check;
    compute_hash(buffer, size, &check);

    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    // Parse header
    char *header_end = memchr(buffer, '\0', size);
    if (!header_end) {
        free(buffer);
        return -1;
    }

    char type_str[10];
    size_t data_size;

    sscanf((char *)buffer, "%s %zu", type_str, &data_size);

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    unsigned char *data_start = (unsigned char *)header_end + 1;

    *data_out = malloc(data_size);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, data_start, data_size);
    *len_out = data_size;

    free(buffer);
    return 0;
}