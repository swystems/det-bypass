#include "args.h"

#if __SERVER__
void ib_print_usage (char *prog)
{
    printf ("Usage: %s -d <ibname> -g <gidx> -p <packets>\n", prog);
    printf ("\t-d, --dev <ibname>\tInterface to attach XDP program to.\n");
    printf ("\t-g, --gidx <gidx>\tGroup index to attach XDP program to.\n");
    printf ("\t-p, --packets <packets>\tNumber of packets to process in the experiment.\n");
}
#else
void ib_print_usage (char *prog)
{
    printf ("Usage: %s -d <ibname> -g <gidx> -p <packets> -i <interval> -s <server_ip>\n", prog);
    printf ("\t-d, --dev <ibname>\tInterface to attach XDP program to.\n");
    printf ("\t-g, --gidx <gidx>\tGroup index to attach XDP program to.\n");
    printf ("\t-p, --packets <packets>\tNumber of packets to process in the experiment.\n");
    printf ("\t-i, --interval <interval>\tInterval between each packet in nanoseconds.\n");
    printf ("\t-s, --server <server_ip>\tServer IP address.\n");
}
#endif

#if __SERVER__
static struct option long_options[] = {
    {"dev", required_argument, 0, 'd'},
    {"gidx", required_argument, 0, 'g'},
    {"packets", required_argument, 0, 'p'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

bool ib_parse_args (int argc, char **argv, char **ibname, int *gidx, uint32_t *iters)
{
    int opt;
    *iters = 0;

    while ((opt = getopt_long (argc, argv, "d:g:p:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'd':
            *ibname = optarg;
            break;
        case 'g':
            *gidx = atoi (optarg);
            break;
        case 'p':
            *iters = atoi (optarg);
            break;
        case 'h':
            return false;
        default:
            return false;
        }
    }

    if (*iters == 0 || *ibname == NULL || *gidx < 0)
        return false;

    return true;
}

#else

static struct option long_options[] = {
    {"dev", required_argument, 0, 'd'},
    {"gidx", required_argument, 0, 'g'},
    {"packets", required_argument, 0, 'p'},
    {"interval", required_argument, 0, 'i'},
    {"server", required_argument, 0, 's'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

bool ib_parse_args (int argc, char **argv, char **ibname, int *gidx, uint32_t *iters, uint64_t *interval, char **server_ip)
{
    int opt;
    *iters = 0;
    *interval = 0;
    *server_ip = NULL;

    while ((opt = getopt_long (argc, argv, "d:g:p:i:s:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'd':
            *ibname = optarg;
            break;
        case 'g':
            *gidx = atoi (optarg);
            break;
        case 'p':
            *iters = atoi (optarg);
            break;
        case 'i':
            *interval = atoi (optarg);
            break;
        case 's':
            *server_ip = optarg;
            break;
        case 'h':
            return false;
        default:
            return false;
        }
    }

    if (*iters == 0 || *ibname == NULL || *gidx < 0)
        return false;

    return true;
}
#endif