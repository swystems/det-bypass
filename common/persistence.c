#include "persistence.h"

__always_inline int bucket_idx (int64_t val, int64_t min, int64_t max, size_t num_buckets)
{
    uint64_t _max = max > 0 ? max : 0;
    uint64_t _min = min < 0 ? min : 0;

    size_t bucket_size = (_max - _min) / num_buckets;
    size_t idx = (val - _min) / bucket_size;

    return idx;
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

    for (int i = 0; i < 4; i++)
    {
        int idx = bucket_idx (ts_diff[i], aux->send_interval - OFFSET, aux->send_interval + OFFSET, NUM_BUCKETS);
        if (idx >= 0 && idx < NUM_BUCKETS)
        {
            LOG (stderr, "ERROR: Packet %d has relative latency %lu, which is out of bounds\n", payload->id, ts_diff[i]);
            return 0;// not really a big deal, we just ignore that round, but we still flag it
        }
    }

    for (int i = 0; i < 4; ++i)
    {
        int idx = bucket_idx (ts_diff[i], aux->send_interval - OFFSET, aux->send_interval + OFFSET, NUM_BUCKETS);
        if (idx >= 0 && idx < NUM_BUCKETS)
        {
            LOG (stderr, "ERROR: Packet %d has absolute latency %lu, which is out of bounds\n", payload->id, ts_diff[i]);
        }
        aux->buckets_rel_latency[i][idx]++;
    }

    uint64_t abs_latency = compute_latency (payload);
    int idx = bucket_idx (abs_latency, 0, aux->send_interval + 2 * OFFSET, NUM_BUCKETS);
    aux->buckets_abs_latency[idx]++;

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
        fprintf (agent->data->file, "%lu %lu %lu %lu\n", aux->buckets_rel_latency[0][i], aux->buckets_rel_latency[1][i], aux->buckets_rel_latency[2][i], aux->buckets_rel_latency[3][i]);
    }
    fprintf (agent->data->file, "---\n");
    for (size_t i = 0; i < NUM_BUCKETS; ++i)
    {
        fprintf (agent->data->file, "%lu\n", aux->buckets_abs_latency[i]);
    }

    free (aux->buckets_abs_latency);
    for (int i = 0; i < 4; i++)
    {
        free (aux->buckets_rel_latency[i]);
    }
    free (aux);

    return persistence_close (agent);
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

    if (agent->flags & PERSISTENCE_M_MIN_MAX_LATENCY)
    {
        struct min_max_latency_data *aux = malloc (sizeof (struct min_max_latency_data));
        if (aux == NULL)
        {
            LOG (stderr, "ERROR: Could not allocate memory for min_max_latency_data\n");
            return -1;
        }
        aux->min = UINT64_MAX;
        aux->max = 0;
        data->aux = aux;

        agent->write = persistence_write_min_max_latency;
        agent->close = persistence_close_min_max;
    }
    else if (agent->flags & PERSISTENCE_M_BUCKETS)
    {
        uint64_t interval = *(uint64_t *) init_aux;
        struct bucket_data *aux = calloc (1, sizeof (struct bucket_data));
        if (aux == NULL)
        {
            LOG (stderr, "ERROR: Could not allocate memory for bucket_data\n");
            return -1;
        }
        aux->send_interval = interval;

        for (int i = 0; i < 4; i++)
        {
            aux->buckets_rel_latency[i] = calloc (NUM_BUCKETS, sizeof (uint64_t));
            if (aux->buckets_rel_latency[i] == NULL)
            {
                LOG (stderr, "ERROR: Could not allocate memory for buckets_abs_latency\n");
                return -1;
            }
        }
        aux->buckets_abs_latency = calloc (NUM_BUCKETS, sizeof (uint64_t));
        if (aux->buckets_abs_latency == NULL)
        {
            LOG (stderr, "ERROR: Could not allocate memory for buckets_rel_latency\n");
            return -1;
        }
        data->aux = aux;

        agent->write = persistence_write_buckets;
        agent->close = persistence_close_buckets;
    }
    else
    {
        agent->write = persistence_write_all_timestamps;
        agent->close = persistence_close;

        data->aux = NULL;
    }

    data->file = file;
    agent->data = data;

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