#include "xdp-loading.h"

#ifndef BPF_F_XDP_DEV_BOUND_ONLY
#define BPF_F_XDP_DEV_BOUND_ONLY (1U << 6)
#endif

struct bpf_object *read_xdp_file (const char *filename)
{
    return bpf_object__open_file (filename, NULL);
}

/**
 * Bind the given XDP program to the given interface.
 * In order to allow the XDP program to access each packet metadata, the program must be "device-bound".
 * This is done by setting the ifindex and the BPF_F_XDP_DEV_BOUND_ONLY flag.
 *
 * @param obj the (not loaded yet!) bpf_object to bind
 * @param ifindex the interface index to bind the program to
 * @param prog_name the name of the program to bind
 * @return a pointer to the bpf_program to bind, NULL on error
 */
struct bpf_program *xdp_bind_to_device (struct bpf_object *obj, const int ifindex, const char *prog_name)
{
    struct bpf_program *prog = bpf_object__find_program_by_name (obj, prog_name);
    if (!prog)
    {
        PERROR ("bpf_object__find_program_by_name");
        return NULL;
    }

    bpf_program__set_ifindex (prog, ifindex);
    bpf_program__set_flags (prog, BPF_F_XDP_DEV_BOUND_ONLY);

    return prog;
}

int attach_xdp (struct bpf_object *obj, const char *prog_name, int ifindex, const char *pinpath)
{
    int ret;

    struct bpf_program *prog = xdp_bind_to_device (obj, ifindex, prog_name);

    ret = bpf_object__load (obj);
    if (ret)
    {
        PERROR ("bpf_object__load");
        return -1;
    }

    if (pinpath)
    {
        // check whether the program is pinned already
        ret = bpf_program__pin (prog, pinpath);
        if (ret)
        {
            PERROR ("bpf_program__pin");
            return -1;
        }
    }

    ret = bpf_xdp_attach (ifindex, bpf_program__fd (prog), XDP_FLAGS_DRV_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST, 0);
    if (ret)
    {
        PERROR ("bpf_xdp_attach");
        return -1;
    }

    return 0;
}

int detach_xdp (struct bpf_object *obj, const char *prog_name, int ifindex, const char *pinpath)
{
    int ret;

    struct bpf_program *prog = xdp_bind_to_device(obj, ifindex, prog_name);
    if (!prog)
    {
        PERROR ("bpf_object__find_program_by_name");
        return -1;
    }
    ret = bpf_object__load (obj);
    if (ret)
    {
        PERROR ("bpf_object__load");
        return -1;
    }

    if (pinpath)
    {
        ret = bpf_program__unpin (prog, pinpath);
        if (ret)
        {
            PERROR ("bpf_program__unpin");
            return -1;
        }

        // try to unpin all the maps of the program
        struct bpf_map *map;
        bpf_object__for_each_map (map, obj)
        {
            char map_pinpath[12 + strlen (bpf_map__name (map)) + 1];// "/sys/fs/bpf/" + map_name + '\0'
            snprintf (map_pinpath, sizeof (map_pinpath), "/sys/fs/bpf/%s", bpf_map__name (map));
            ret = bpf_map__unpin (map, map_pinpath);
            if (ret)
            {
                PERROR ("bpf_map__unpin");
            }
        }
    }

    ret = bpf_xdp_detach (ifindex, XDP_FLAGS_DRV_MODE, 0);
    if (ret)
    {
        PERROR ("bpf_xdp_detach");
        return -1;
    }

    return 0;
}

void *mmap_bpf_map (struct bpf_object *loaded_xdp_obj, const char *mapname, const size_t map_size)
{
    int map_fd = bpf_object__find_map_fd_by_name (loaded_xdp_obj, mapname);
    if (map_fd < 0)
    {
        LOG (stderr, "ERR: finding map failed\n");
        return NULL;
    }

    void *map = mmap (NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, map_fd, 0);
    if (map == MAP_FAILED)
    {
        LOG (stderr, "ERR: mmap failed\n");
        return NULL;
    }

    return map;
}