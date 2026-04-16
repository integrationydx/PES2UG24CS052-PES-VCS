#ifndef OBJECT_H
#define OBJECT_H

#include <stdint.h>
#include <stddef.h>

// Define the hash size (Git uses 20 for SHA-1, but your code suggests 32)
#define HASH_SIZE 32

typedef struct {
    uint8_t hash[HASH_SIZE];
} ObjectID;

typedef enum {
    OBJ_COMMIT,
    OBJ_TREE,
    OBJ_BLOB
} ObjectType;

// Function prototype for writing objects
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Add any other function prototypes that are defined in object.c
// so that other files can see them.

#endif