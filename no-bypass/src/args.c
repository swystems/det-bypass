#include "args.h"

#if __SERVER__
void nobypass_print_usage (char *prog)
{
    printf ("Usage: %s -p <packets>\n", prog);
    printf ("\t-p, --packets <packets>\tNumber of packets to process in the experiment.\n");
}
#else
void nobypass_print_usage (char *prog)
{
    printf ("Usage: %s -p <packets> -i <interval> -s <server_ip> [-m <measurement>]\n", prog);
    printf ("\t-p, --packets <packets>\tNumber of packets to process in the experiment.\n");
    printf ("\t-i, --interval <interval>\tInterval between each packet in nanoseconds.\n");
    printf ("\t-s, --server <server_ip>\tServer IP address.\n");
    printf ("\t-m, --measurement <measurement>\tMeasurement to perform. 0: All Timestamps, 1: Min/Max latency, 2: Buckets.\n");
}
#endif

#if __SERVER__
static struct option long_options[] = {
    {"packets", required_argument, 0, 'p'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

bool nobypass_parse_args (int argc, char **argv, uint32_t *iters)
{
    int opt;
    *iters = 0;

    while ((opt = getopt_long (argc, argv, "p:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'p':
            *iters = atoi (optarg);
            break;
        case 'h':
            return false;
        default:
            return false;
        }
    }

    if (*iters == 0)
        return false;

    return true;
}
#else
static struct option long_options[] = {
    {"packets", required_argument, 0, 'p'},
    {"interval", required_argument, 0, 'i'},
    {"server", required_argument, 0, 's'},
    {"help", no_argument, 0, 'h'},
    {"measurement", required_argument, 0, 'm'},
    {0, 0, 0, 0}};

bool nobypass_parse_args (int argc, char **argv, uint32_t *iters, uint64_t *interval, char **server_ip, uint32_t *pers_flags)
{
    int opt;
    *iters = 0;
    *interval = 0;
    *server_ip = NULL;

    while ((opt = getopt_long (argc, argv, "p:i:s:hm:", long_options, NULL)) != -1)
    {
        switch (opt)
        {
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
        case 'm':
            *pers_flags = pers_measurement_to_flag (atoi (optarg));
            break;
        default:
            return false;
        }
    }

    if (*iters == 0 || *interval == 0 || *server_ip == NULL)
        return false;

    return true;
}
#endif