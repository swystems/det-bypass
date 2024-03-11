#pragma once

#include "../../common/common.h"
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ib_print_usage (char *prog);

#if __SERVER__
bool ib_parse_args (int argc, char **argv, char **ibname, int *gidx, uint32_t *iters);
#else
bool ib_parse_args (int argc, char **argv, char **ibname, int *gidx, uint32_t *iters, uint64_t *interval, char **server_ip);
#endif
