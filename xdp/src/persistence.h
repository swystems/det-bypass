#pragma once

#include "../common.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>

enum persistence_flags {
    // Use a threaded persistence agent
    PERS_F_THREADED = (1 << 1),
};

/**
 * Data used by a base file persistence agent.
 */
typedef struct pers_base_data {
    FILE *file;
} pers_base_data_t;

/**
 * Data used by a threaded persistence agent.
 *
 * The file pointer must be the first element of the struct to allow casting
 * to pers_base_data_t.
 */
typedef struct pers_data_threaded {
    FILE *file;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool has_data;
    struct pingpong_payload payload;
    volatile bool stop;
} pers_data_threaded_t;

/**
 * "Persistence agent" to be used to store information about the pingpong measurements.
 */
typedef struct persistence_agent {
    /**
     * Pointer to the data structure used by the persistence agent depending on the flags.
     * This pointer is owned by the persistence agent and should not be freed by the user.
     */
    pers_base_data_t *data;

    /**
     * Write data to the persistence agent.
     *
     * @param agent the agent to use
     * @param data the data to write
     * @return 0 on success, -1 on error
     */
    int (*write) (struct persistence_agent *agent, const struct pingpong_payload *data);

    /**
     * Close and cleanup the persistence agent and its data.
     * This function takes ownership of the pointer and frees it. After this function returns,
     * the pointer is no longer valid.
     *
     * @param agent the persistence agent to close
     * @return 0 on success, -1 on error
     */
    int (*close) (struct persistence_agent *agent);
} persistence_agent_t;

/**
 * Initialize persistence module.
 * This function should be called before any other persistence function and only once.
 *
 * @param filename the name of the file to store data
 * @return 0 on success, -1 on error
 */
persistence_agent_t *persistence_init (const char *filename, uint8_t flags);