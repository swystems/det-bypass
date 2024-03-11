#pragma once

#include "../../common/common.h"
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void xdp_print_usage (char *prog);

#if __SERVER__
bool xdp_parse_args (int argc, char **argv, char **ifname, bool *remove, uint32_t *iters);
#else
bool xdp_parse_args (int argc, char **argv, char **ifname, bool *remove, uint32_t *iters, uint64_t *interval, char **server_ip);
#endif