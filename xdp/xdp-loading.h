#pragma once

#include "common.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <linux/types.h>

/**
 * Open an XDP filter file and return the corresponding bpf_object.
 *
 * @param filename The path to the XDP filter file.
 * @return A pointer to the bpf_object corresponding to the XDP filter file.
 */
struct bpf_object *read_xdp_file (const char *filename);

/**
 * Load the XDP program and attach it to the given interface.
 * If `pinpath` is not NULL, the program is pinned to the given path.
 *
 * @param obj the bpf_object to load
 * @param prog_name the name of the program to load
 * @param ifindex the interface index to attach the program to
 * @param pinpath the path to pin the program to
 * @return 0 on success, a negative value on error
 */
int attach_xdp (struct bpf_object *obj, const char *prog_name, int ifindex, const char *pinpath);

/**
 * Detach the XDP program from the given interface.
 * If `pinpath` is not NULL, the program is unpinned from the given path.
 *
 * @param obj the bpf_object to load
 * @param prog_name the name of the program to load
 * @param ifindex the interface index to detach the program from
 * @param pinpath the path to unpin the program from
 * @return 0 on success, a negative value on error
 */
int detach_xdp (struct bpf_object *obj, const char *prog_name, int ifindex, const char *pinpath);
