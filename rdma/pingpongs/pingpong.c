/*
 * Copyright (c) 2006 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pingpong.h"
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum ibv_mtu pp_mtu_to_enum (int mtu)
{
    switch (mtu)
    {
    case 256:
        return IBV_MTU_256;
    case 512:
        return IBV_MTU_512;
    case 1024:
        return IBV_MTU_1024;
    case 2048:
        return IBV_MTU_2048;
    case 4096:
        return IBV_MTU_4096;
    default:
        return 0;
    }
}

int pp_get_port_info (struct ibv_context *context, int port, struct ibv_port_attr *attr)
{
    return ibv_query_port (context, port, attr);
}

/**
 * Convert a wire gid string to a gid structure.
 *
 * @param wgid the wire gid string
 * @param gid the gid structure
 */
void wire_gid_to_gid (const char *wgid, union ibv_gid *gid)
{
    char tmp[9];
    __be32 v32;
    int i;
    uint32_t tmp_gid[4];

    for (tmp[8] = 0, i = 0; i < 4; ++i)
    {
        memcpy (tmp, wgid + i * 8, 8);
        sscanf (tmp, "%x", &v32);
        tmp_gid[i] = be32toh (v32);
    }
    memcpy (gid, tmp_gid, sizeof (*gid));
}

/**
 * Convert a gid structure to a gid string to be transmitted.
 *
 * @param gid the gid structure
 * @param wgid the wire gid string
 */
void gid_to_wire_gid (const union ibv_gid *gid, char wgid[])
{
    uint32_t tmp_gid[4];
    int i;

    memcpy (tmp_gid, gid, sizeof (tmp_gid));
    for (i = 0; i < 4; ++i)
        sprintf (&wgid[i * 8], "%08x", htobe32 (tmp_gid[i]));
}

long long get_nanos (void)
{
    struct timespec ts;
    timespec_get (&ts, TIME_UTC);
    return (long long) ts.tv_sec * 1000000000L + ts.tv_nsec;
}

struct pingpong_data *init_pingpong_data (int num_payloads)
{
    struct pingpong_data *data = malloc (sizeof (struct pingpong_data));
    if (data == NULL)
    {
        perror ("malloc");
        exit (EXIT_FAILURE);
    }

    data->payloads = malloc (num_payloads * sizeof (struct pingpong_payload));
    if (data->payloads == NULL)
    {
        perror ("malloc");
        exit (EXIT_FAILURE);
    }

    data->num_payloads = 0;

    return data;
}

void free_pingpong_data (struct pingpong_data *data)
{
    free (data->payloads);
    free (data);
}

void print_payload (struct pingpong_payload *payload)
{
    printf ("%lu\t%lu\t%lu\t%lu\n", payload->ts[0], payload->ts[1], payload->ts[2], payload->ts[3]);
}

/**
 * Update the payload with the current timestamp.
 * Stage is the current stage of the pingpong packet between 1 and 4 inclusive.
 *
 * @param payload the payload to update
 * @param stage the stage of the payload
 */
void update_payload (struct pingpong_payload *payload, int stage)
{
    if (stage < 1 || stage > 4)
    {
        fprintf (stderr, "update_payload: stage must be between 1 and 4 inclusive\n");
        exit (EXIT_FAILURE);
    }
    payload->ts[stage - 1] = get_nanos ();
}

void store_payload (struct pingpong_payload *payload, struct pingpong_data *data)
{
    data->payloads[data->num_payloads] = *payload;
    data->num_payloads++;
}

/**
 * Save the payloads to a file.
 *
 * @param payloads the payloads to save
 * @param num_payloads the number of payloads to save
 * @param filename the name of the file to save to
 */
void save_payloads_to_file (struct pingpong_data *data, unsigned int warmup, const char *foldername)
{
    // create folder "results" if it doesn't exist
    char command[100];
    sprintf (command, "mkdir -p %s", foldername);
    system (command);

    printf ("Total number of payloads: %d\n", data->num_payloads);
    printf ("Number of warmup payloads: %d\n", warmup);
    printf ("Storing %d payloads to file...\n", data->num_payloads - warmup);

    // filename is "{foldername}/payloads.txt"
    char filename[100];
    sprintf (filename, "%s/payloads.txt", foldername);
    FILE *fp = fopen (filename, "w");
    if (fp == NULL)
    {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    for (unsigned int i = warmup; i < data->num_payloads; i++)
    {
        fprintf (fp, "%lu\t%lu\t%lu\t%lu\n", data->payloads[i].ts[0], data->payloads[i].ts[1], data->payloads[i].ts[2],
                 data->payloads[i].ts[3]);
    }

    fclose (fp);

    sprintf (filename, "%s/latencies.txt", foldername);
    fp = fopen (filename, "w");
    if (fp == NULL)
    {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    for (unsigned int i = warmup; i < data->num_payloads; i++)
    {
        double latency = (((double) data->payloads[i].ts[3] - data->payloads[i].ts[0]) - ((double) data->payloads[i].ts[2] - data->payloads[i].ts[1])) / 2;
        fprintf (fp, "%f\n", latency);
    }

    fclose (fp);
}