#include "persistence.h"

int pers_file_write (persistence_agent_t *agent, const struct pingpong_payload *payload)
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

int pers_file_close (persistence_agent_t *agent)
{
    if (!agent->data || !agent->data->file)
    {
        LOG (stderr, "ERROR: Persistence module not initialized\n");
        return -1;
    }

    if (fclose (agent->data->file) != 0)
    {
        LOG (stderr, "ERROR: Could not close persistence file\n");
        return -1;
    }

    free (agent->data);
    free (agent);

    return 0;
}

int pers_file_init (persistence_agent_t *agent, const char *filename)
{
    FILE *file = fopen (filename, "w");
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

    agent->write = pers_file_write;
    agent->close = pers_file_close;

    return 0;
}

void *pers_threaded_thread (void *aux)
{
    persistence_agent_t *agent = (persistence_agent_t *) aux;
    pers_data_threaded_t *data = (pers_data_threaded_t *) agent->data;

    while (!data->stop)
    {
        pthread_testcancel ();
        pthread_mutex_lock (&data->mutex);
        while (!data->has_data && !data->stop)
        {
            pthread_cond_wait (&data->cond, &data->mutex);
        }
        if (data->stop)
        {
            pthread_mutex_unlock (&data->mutex);
            break;
        }

        if (pers_file_write (agent, &data->payload) != 0)
        {
            LOG (stderr, "ERROR: Could not write to persistence file\n");
            return NULL;
        }

        data->has_data = false;
        pthread_mutex_unlock (&data->mutex);
    }

    return NULL;
}

int pers_threaded_write (persistence_agent_t *agent, const struct pingpong_payload *payload)
{
    pers_data_threaded_t *data = (pers_data_threaded_t *) agent->data;

    pthread_mutex_lock (&data->mutex);
    data->payload = *payload;
    data->has_data = true;
    pthread_cond_signal (&data->cond);
    pthread_mutex_unlock (&data->mutex);

    return 0;
}

int pers_threaded_close (persistence_agent_t *agent)
{
    pers_data_threaded_t *data = (pers_data_threaded_t *) agent->data;

    LOG (stdout, "Cleaning up persistence threaded module... ");

    pthread_mutex_lock (&data->mutex);
    data->stop = true;
    pthread_cond_signal (&data->cond);
    pthread_mutex_unlock (&data->mutex);

    LOG (stdout, "OK\n");

    if (pthread_join (data->thread, NULL) != 0)
    {
        LOG (stderr, "ERROR: Could not join thread\n");
        return -1;
    }

    if (fclose (data->file) != 0)
    {
        LOG (stderr, "ERROR: Could not close persistence file\n");
        return -1;
    }

    int ret = pthread_mutex_destroy (&data->mutex);
    if (ret != 0)
    {
        LOG (stderr, "ERROR: Could not destroy mutex; %d\n", ret);
        return -1;
    }

    if (pthread_cond_destroy (&data->cond) != 0)
    {
        LOG (stderr, "ERROR: Could not destroy condition variable\n");
        return -1;
    }

    free (agent->data);
    free (agent);

    return 0;
}

int pers_threaded_init (persistence_agent_t *agent, const char *filename)
{
    pers_data_threaded_t *data = malloc (sizeof (pers_data_threaded_t));

    agent->data = (pers_base_data_t *) data;
    agent->write = pers_threaded_write;
    agent->close = pers_threaded_close;

    if (data == NULL)
    {
        LOG (stderr, "ERROR: Could not allocate memory for persistence data\n");
        return -1;
    }

    FILE *file = fopen (filename, "w");
    if (file == NULL)
    {
        LOG (stderr, "ERROR: Could not open persistence file\n");
        return -1;
    }
    data->file = file;

    if (pthread_mutex_init (&data->mutex, NULL) != 0)
    {
        LOG (stderr, "ERROR: Could not initialize mutex\n");
        return -1;
    }

    if (pthread_cond_init (&data->cond, NULL) != 0)
    {
        LOG (stderr, "ERROR: Could not initialize condition variable\n");
        return -1;
    }

    data->has_data = false;
    data->stop = false;

    if (pthread_create (&data->thread, NULL, pers_threaded_thread, agent) != 0)
    {
        LOG (stderr, "ERROR: Could not create thread\n");
        return -1;
    }

    return 0;
}

persistence_agent_t *persistence_init (const char *filename, uint8_t flags)
{
    persistence_agent_t *agent = malloc (sizeof (persistence_agent_t));

    if (flags & PERS_F_THREADED)
    {
        if (pers_threaded_init (agent, filename) != 0)
        {
            return NULL;
        }
    }
    else
    {
        if (pers_file_init (agent, filename) != 0)
        {
            return NULL;
        }
    }

    return agent;
}