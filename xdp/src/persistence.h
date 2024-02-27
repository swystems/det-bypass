#pragma once

#include "../common.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>

enum persistence_output_flags {
    PERSISTENCE_F_FILE = 1 << 0,
    PERSISTENCE_F_STDOUT = 1 << 1,
};

/**
 * Flags defining what should be persisted.
 */
enum persistence_data_flags {
    // Store all rounds timestamps. Default option.
    PERSISTENCE_F_ALL_TIMESTAMPS = 1 << 2,
    // Store rounds with minimum and maximum latency
    PERSISTENCE_F_MIN_MAX_LATENCY = 1 << 3,
    // TODO: Store rounds in buckets
    PERSISTENCE_F_BUCKETS = 1 << 4,
};

struct min_max_latency_data {
    uint64_t min;
    uint64_t max;
    struct pingpong_payload min_payload;
    struct pingpong_payload max_payload;
};

/**
 * Data used by a base file persistence agent.
 */
typedef struct pers_base_data {
    // Output stream to write to
    FILE *file;

    /**
     * Auxiliary data, depending on the flags.
     * - PERSISTENCE_F_ALL_TIMESTAMPS: NULL
     * - PERSISTENCE_F_MIN_MAX_LATENCY: struct min_max_latency_data
     * - PERSISTENCE_F_BUCKETS: struct bucket_data
     */
    void *aux;
} pers_base_data_t;

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
     * Flags passed to the initialization function.
     */
    int flags;

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