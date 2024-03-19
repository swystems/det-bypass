#include "persistence.h"

__always_inline uint32_t bucket_idx (const int64_t val, const int64_t min, const int64_t max)
{
    const int64_t bucket_size = (max - min) / NUM_BUCKETS;
    if (UNLIKELY (val < min || val > max))
    {
        return -1;
    }
    return (val - min) / bucket_size;
}

__always_inline int bucket_compute_hugepage_size ()
{
    const uint64_t page_size = sysconf (_SC_PAGESIZE);
    // 4 arrays of NUM_BUCKETS elements (relative latencies), plus 1 array of NUM_BUCKETS elements (absolute latencies)
    // + page_size - 1 to round up
    const uint32_t required_pages = (sizeof (uint64_t) * NUM_BUCKETS * 5 + page_size - 1) / page_size;
    return required_pages * page_size;
}

int persistence_write_all_timestamps (persistence_agent_t *agent, const struct pingpong_payload *payload)
{
    if (!agent->data || !agent->data->file)
    {
        LOG (stderr, "ERROR: Persistence agent not initialized\n");
        return -1;
    }

    // print the paylaod id to file
    if (fprintf (agent->data->file, "%d: %llu %llu %llu %llu\n", payload->id, payload->ts[0], payload->ts[1], payload->ts[2], payload->ts[3]) < 0)
    {
        LOG (stderr, "ERROR: Could not write to persistence file\n");
        return -1;
    }

    return 0;
}

int persistence_write_min_max_latency (persistence_agent_t *agent, const struct pingpong_payload *payload)
{
    struct min_max_latency_data *aux = agent->data->aux;
    const uint64_t latency = compute_latency (payload);
    if (latency < aux->min)
    {
        aux->min = latency;
        aux->min_payload = *payload;
    }
    if (latency > aux->max)
    {
        aux->max = latency;
        aux->max_payload = *payload;
    }
    return 0;
}

int persistence_write_buckets (persistence_agent_t *agent, const struct pingpong_payload *payload)
{
    struct bucket_data *aux = agent->data->aux;
    if (!valid_pingpong_payload (&aux->prev_payload))
    {
        aux->prev_payload = *payload;
        return -1;
    }

    uint64_t ts_diff[4];
    for (int i = 0; i < 4; i++)
    {
        ts_diff[i] = payload->ts[i] - aux->prev_payload.ts[i];
    }

    const uint64_t rel_min = aux->send_interval - OFFSET;
    const uint64_t rel_max = aux->send_interval + OFFSET;
    const uint64_t abs_min = 0;
    const uint64_t abs_max = aux->send_interval + 2 * OFFSET;

    /* Check if all relative latencies are within bounds. Provides some sort of atomicity. */
    for (int i = 0; i < 4; ++i)
    {
        const uint32_t idx = bucket_idx (ts_diff[i], rel_min, rel_max);
        if (UNLIKELY (idx >= NUM_BUCKETS))
        {
            fprintf (stderr, "%d: %lu %lu %lu %lu\n", payload->id, ts_diff[0], ts_diff[1], ts_diff[2], ts_diff[3]);
            goto exit;// it's ok, we just skip the packet but notify the user.
        }
    }

    for (int i = 0; i < 4; ++i)
    {
        const uint32_t idx = bucket_idx (ts_diff[i], rel_min, rel_max);
        aux->buckets[idx].rel_latency[i]++;
    }

    const uint64_t abs_latency = compute_latency (payload);

    const uint32_t idx = bucket_idx (abs_latency, abs_min, abs_max);
    if (idx < NUM_BUCKETS)
    {
        aux->buckets[idx].abs_latency++;
    }
    else
    {
        fprintf (stderr, "%d: %lu\n", payload->id, abs_latency);
    }

exit:
    aux->prev_payload = *payload;
    return 0;
}

int persistence_close (persistence_agent_t *agent)
{
    if (!agent->data || !agent->data->file)
    {
        LOG (stderr, "ERROR: Persistence module not initialized\n");
        return -1;
    }

    if (agent->data->file != stdout && fclose (agent->data->file) != 0)
    {
        LOG (stderr, "ERROR: Could not close persistence file\n");
        return -1;
    }

    free (agent->data);
    free (agent);

    return 0;
}

int persistence_close_min_max (persistence_agent_t *agent)
{
    struct min_max_latency_data *aux = agent->data->aux;
    if (aux->min != UINT64_MAX)
    {
        struct pingpong_payload *payload = &aux->min_payload;
        fprintf (agent->data->file, "%d: %llu %llu %llu %llu (LATENCY %lu ns)\n", payload->id, payload->ts[0], payload->ts[1], payload->ts[2], payload->ts[3], aux->min);
    }
    if (aux->max != 0)
    {
        struct pingpong_payload *payload = &aux->max_payload;
        fprintf (agent->data->file, "%d: %llu %llu %llu %llu (LATENCY %lu ns)\n", payload->id, payload->ts[0], payload->ts[1], payload->ts[2], payload->ts[3], aux->max);
    }

    free (aux);

    return persistence_close (agent);
}

int persistence_close_buckets (persistence_agent_t *agent)
{
    struct bucket_data *aux = agent->data->aux;

    for (size_t i = 0; i < NUM_BUCKETS; ++i)
    {
        fprintf (agent->data->file, "%lu %lu %lu %lu %lu\n",
                 aux->buckets[i].rel_latency[0],
                 aux->buckets[i].rel_latency[1],
                 aux->buckets[i].rel_latency[2],
                 aux->buckets[i].rel_latency[3],
                 aux->buckets[i].abs_latency);
    }

    munmap (aux->ptr, bucket_compute_hugepage_size ());
    free (aux);

    return persistence_close (agent);
}

int persistence_init_min_max_latency (persistence_agent_t *agent, void *init_aux __unused)
{
    struct min_max_latency_data *aux = malloc (sizeof (struct min_max_latency_data));
    if (aux == NULL)
    {
        LOG (stderr, "ERROR: Could not allocate memory for min_max_latency_data\n");
        return -1;
    }
    aux->min = UINT64_MAX;
    aux->max = 0;
    agent->data->aux = aux;

    agent->write = persistence_write_min_max_latency;
    agent->close = persistence_close_min_max;
    return 0;
}

int persistence_init_buckets (persistence_agent_t *agent, void *init_aux)
{
    const uint64_t interval = *(const uint64_t *) init_aux;
    struct bucket_data *aux = calloc (1, sizeof (struct bucket_data));
    if (aux == NULL)
    {
        LOG (stderr, "ERROR: Could not allocate memory for bucket_data\n");
        return -1;
    }
    aux->send_interval = interval;

    uint64_t memory_size = bucket_compute_hugepage_size ();
    // Allocate memory for the buckets on a huge page
    void *ptr = mmap (NULL, memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_LOCKED, -1, 0);
    if (ptr == MAP_FAILED)
    {
        LOG (stderr, "ERROR: Could not allocate memory for buckets_rel_latency\n");
        return -1;
    }

    // Lock the memory to avoid major page faults
    mlock (ptr, memory_size);

    aux->ptr = ptr;

    agent->data->aux = aux;

    agent->write = persistence_write_buckets;
    agent->close = persistence_close_buckets;
    return 0;
}

int _persistence_init (persistence_agent_t *agent, const char *filename, void *init_aux)
{
    bool use_stdout = filename == NULL || (agent->flags & PERSISTENCE_F_STDOUT);
    FILE *file = use_stdout ? stdout : fopen (filename, "w");
    if (file == NULL)
    {
        LOG (stderr, "ERROR: Could not open persistence file\n");
        return -1;
    }

    pers_base_data_t *data = malloc (sizeof (pers_base_data_t));
    if (data == NULL)
    {
        LOG (stderr, "ERROR: Could not allocate memory for persistence data\n");
        return -1;
    }

    data->file = file;
    agent->data = data;

    if (agent->flags & PERSISTENCE_M_MIN_MAX_LATENCY)
    {
        if (persistence_init_min_max_latency (agent, init_aux) != 0)
        {
            return -1;
        }
    }
    else if (agent->flags & PERSISTENCE_M_BUCKETS)
    {
        if (persistence_init_buckets (agent, init_aux) != 0)
        {
            return -1;
        }
    }
    else
    {
        agent->write = persistence_write_all_timestamps;
        agent->close = persistence_close;

        data->aux = NULL;
    }

    return 0;
}

persistence_agent_t *persistence_init (const char *filename, uint32_t flags, void *aux)
{
    persistence_agent_t *agent = malloc (sizeof (persistence_agent_t));
    agent->flags = flags;

    if (_persistence_init (agent, filename, aux) != 0)
    {
        return NULL;
    }

    return agent;
}