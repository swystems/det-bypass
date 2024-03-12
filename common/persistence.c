#include "persistence.h"

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

int _persistence_init (persistence_agent_t *agent, const char *filename)
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

persistence_agent_t *persistence_init (const char *filename, uint32_t flags)
{
    persistence_agent_t *agent = malloc (sizeof (persistence_agent_t));
    agent->flags = flags;

    if (_persistence_init (agent, filename) != 0)
    {
        return NULL;
    }

    return agent;
}