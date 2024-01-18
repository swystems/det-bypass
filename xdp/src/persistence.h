#pragma once

#include "../common.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Initialize persistence module.
 * This function should be called before any other persistence function and only once.
 *
 * @param filename the name of the file to store data
 * @return 0 on success, -1 on error
 */
int persistence_init (const char *filename);

/**
 * Write data to the persistence file.
 *
 * @param data the data to write
 * @param size the size of the data
 * @return 0 on success, -1 on error
 */
int persistence_write (const uint8_t *data, size_t size);

/**
 * Close the persistence module.
 * This function will wait for the thread to finish.
 *
 * @return 0 on success, -1 on error
 */
int persistence_close (void);