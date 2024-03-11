#pragma once

#include "../../common/common.h"
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nobypass_print_usage (char *prog);

#if __SERVER__
bool nobypass_parse_args (int argc, char **argv, uint32_t *iters);
#else
bool nobypass_parse_args (int argc, char **argv, uint32_t *iters, uint64_t *interval, char **server_ip);
#endif