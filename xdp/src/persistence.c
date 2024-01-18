#include "persistence.h"

static FILE *persistence_file;
static bool persistence_inited = false;

int persistence_init (const char *filename)
{
    if (persistence_inited)
    {
        LOG (stderr, "WARNING: Persistence module already initialized\n");
        // allow for multiple calls to persistence_init, but make sure to clean up
        persistence_close ();
    }

    persistence_file = fopen (filename, "w");
    if (persistence_file == NULL)
    {
        LOG (stderr, "ERROR: Could not open persistence file\n");
        return -1;
    }

    persistence_inited = true;
    return 0;
}

int persistence_write (const struct pingpong_payload *payload)
{
    if (!persistence_inited)
    {
        LOG (stderr, "ERROR: Persistence module not initialized\n");
        return -1;
    }

    // print the paylaod id to file
    if (fprintf (persistence_file, "%d\n", payload->id) < 0)
    {
        LOG (stderr, "ERROR: Could not write to persistence file\n");
        return -1;
    }

    return 0;
}

int persistence_close (void)
{
    if (!persistence_inited)
    {
        LOG (stderr, "ERROR: Persistence module not initialized\n");
        return -1;
    }

    if (fclose (persistence_file) != 0)
    {
        LOG (stderr, "ERROR: Could not close persistence file\n");
        return -1;
    }

    persistence_inited = false;
    return 0;
}