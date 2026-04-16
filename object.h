#ifndef OBJECT_H
#define OBJECT_H

#include "pes.h"

// Function prototype for writing objects
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

void compute_hash(const void *data, size_t len, ObjectID *id_out);

void object_path(const ObjectID *id, char *path_out, size_t path_size);

int object_exists(const ObjectID *id);

// Add any other function prototypes that are defined in object.c
// so that other files can see them.

#endif