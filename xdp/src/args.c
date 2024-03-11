#include "args.h"

#if __SERVER__
void xdp_print_usage (char *prog)
{
    printf ("Usage: %s -d <ifname> [--remove] [-p <packets>]\n", prog);
    printf ("\t-r, --remove\tRemove XDP program. Only `ifname` is required.\n");
    printf ("\t-d, --dev <ifname>\tInterface to attach XDP program to.\n");
    printf ("\t-p, --packets <packets>\tNumber of packets to process in the experiment.\n");
}
#else
void xdp_print_usage (char *prog)
{
    printf ("Usage: %s -d <ifname> [--remove] [-p <packets> -i <interval> -s <server_ip>]\n", prog);
    printf ("\t-r, --remove\tRemove XDP program. Only `ifname` is required.\n");
    printf ("\t-d, --dev <ifname>\tInterface to attach XDP program to.\n");
    printf ("\t-p, --packets <packets>\tNumber of packets to process in the experiment.\n");
    printf ("\t-i, --interval <interval>\tInterval between each packet in nanoseconds.\n");
    printf ("\t-s, --server <server_ip>\tServer IP address.\n");
}
#endif

#if __SERVER__
static struct option long_options[] = {
    {"remove", no_argument, 0, 'r'},
    {"dev", required_argument, 0, 'd'},
    {"packets", required_argument, 0, 'p'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

bool xdp_parse_args (int argc, char **argv, char **ifname, bool *remove, uint32_t *iters)
{
    int opt;
    *iters = 0;
    *remove = false;

    while ((opt = getopt_long (argc, argv, "d:p:r:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'd':
            *ifname = optarg;
            break;
        case 'p':
            *iters = atoi (optarg);
            break;
        case 'r':
            *remove = true;
            break;
        case 'h':
            return false;
        default:
            return false;
        }
    }

    if (*ifname == NULL || (!*remove && *iters == 0))
        return false;

    return true;
}

#else

static struct option long_options[] = {
    {"remove", no_argument, 0, 'r'},
    {"dev", required_argument, 0, 'd'},
    {"packets", required_argument, 0, 'p'},
    {"interval", required_argument, 0, 'i'},
    {"server", required_argument, 0, 's'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

bool xdp_parse_args (int argc, char **argv, char **ifname, bool *remove, uint32_t *iters, uint64_t *interval, char **server_ip)
{
    int opt;
    *iters = 0;
    *interval = 0;
    *remove = false;

    while ((opt = getopt_long (argc, argv, "d:p:i:s:r:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'd':
            *ifname = optarg;
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
        case 'r':
            *remove = true;
            break;
        case 'h':
            return false;
        default:
            return false;
        }
    }

    if (*ifname == NULL || (!*remove && (*iters == 0 || *interval == 0 || *server_ip == NULL)))
        return false;

    return true;
}
#endif