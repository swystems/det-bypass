#include "common.h"
#include "src/net.h"
#include "src/xdp-loading.h"

#include <bpf/libbpf.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>

void usage (char *prog)
{
    // <prog> <ifname> <action>
    fprintf (stderr, "Usage: %s <ifname> <action> [extra arguments]\n", prog);
    fprintf (stderr, "Actions:\n");
    fprintf (stderr, "\t- start: start the pingpong experiment\n");
    fprintf (stderr, "\t         only on the client machine, it requires two extra arguments: <number of packets> <server IP>\n");
    fprintf (stderr, "\t- remove: remove XDP program\n");
}

static const char *filename = "pingpong.o";
static const char *prog_name = "xdp_main";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong";
static const char *mapname = "last_timestamp";

static unsigned int iters = 0;

static struct bpf_object *loaded_xdp_obj;
volatile bool is_polling = false;

/**
 * Continuously poll the XDP map for the latest pingpong_payload value and print it.
 */
void *poll_thread (void *aux __attribute__ ((unused)))
{
    int map_fd = bpf_object__find_map_fd_by_name (loaded_xdp_obj, mapname);
    if (map_fd < 0)
    {
        fprintf (stderr, "ERR: finding map failed\n");
        return NULL;
    }

    void *map = mmap (NULL, sizeof (struct pingpong_payload), PROT_READ, MAP_SHARED, map_fd, 0);
    if (map == MAP_FAILED)
    {
        fprintf (stderr, "ERR: mmap failed\n");
        return NULL;
    }

    // notify the program that the polling thread is ready
    is_polling = true;

    uint32_t last_id = 0;
    struct pingpong_payload *payload = map;
    while (last_id < iters - 1)
    {
        printf ("ID %d: %llu %llu %llu %llu\n", payload->id, payload->ts[0], payload->ts[1], payload->ts[2],
                payload->ts[3]);
        last_id = payload->id;

        if (last_id == iters - 1)
            break;

        BUSY_WAIT (payload->id == last_id);
    }

    munmap (map, sizeof (struct pingpong_payload));

    printf ("Poll thread finished\n");
    return NULL;
}

pthread_t start_poll_thread (void)
{
    // Create and start a thread with poll_thread function
    pthread_t thread;
    pthread_create (&thread, NULL, poll_thread, NULL);

    BUSY_WAIT (!is_polling);

    return thread;
}

void start_pingpong (int ifindex, const char *server_ip)
{
    const pthread_t thread = start_poll_thread ();

    send_packets (ifindex, server_ip, iters, 20000);

    pthread_join (thread, NULL);
}

int attach_pingpong_xdp (int ifindex)
{
    struct bpf_object *obj = read_xdp_file (filename);
    if (!obj)
    {
        return -1;
    }

    loaded_xdp_obj = obj;
    return attach_xdp (obj, prog_name, ifindex, pinpath);
}

int detach_pingpong_xdp (int ifindex)
{
    struct bpf_object *obj = read_xdp_file (filename);
    if (!obj)
    {
        return -1;
    }

    return detach_xdp (obj, prog_name, ifindex, pinpath);
}

int main (int argc, char **argv)
{
    if (argc < 3)
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    char *ifname = argv[1];
    char *action = argv[2];

    int ifindex = if_nametoindex (ifname);
    if (!ifindex)
    {
        perror ("if_nametoindex");
        return EXIT_FAILURE;
    }

    if (strcmp (action, "start") == 0)
    {
        detach_pingpong_xdp (ifindex);// always try to detach first

        int ret = attach_pingpong_xdp (ifindex);
        if (ret)
        {
            fprintf (stderr, "ERR: attaching program failed\n");
            return EXIT_FAILURE;
        }
        printf ("XDP program attached\n");

        if (argc == 3)
        {
            // the client does not need to do anything else (for now!)
            return EXIT_SUCCESS;
        }
        else if (argc < 5)
        {
            // if it's not 3, it must be at least 5
            usage (argv[0]);
            return EXIT_FAILURE;
        }

        iters = atoi (argv[3]);
        char *ip = argv[4];

        start_pingpong (ifindex, ip);
    }
    else if (strcmp (action, "remove") == 0)
    {
        int ret = detach_pingpong_xdp (ifindex);
        if (ret)
        {
            fprintf (stderr, "ERR: detaching program failed\n");
            return EXIT_FAILURE;
        }
        printf ("XDP program detached\n");
    }
    else
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}