#include "xdp-loading.h"

struct bpf_object *read_xdp_file (const char *filename)
{
    return bpf_object__open_file (filename, NULL);
}

int attach_xdp (struct bpf_object *obj, const char *prog_name, int ifindex, const char *pinpath)
{
    int ret;

    ret = bpf_object__load (obj);
    if (ret)
    {
        PERROR ("bpf_object__load");
        return -1;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name (obj, prog_name);
    if (!prog)
    {
        PERROR ("bpf_object__find_program_by_name");
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

    ret = bpf_object__load (obj);
    if (ret)
    {
        PERROR ("bpf_object__load");
        return -1;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name (obj, prog_name);
    if (!prog)
    {
        PERROR ("bpf_object__find_program_by_name");
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

    void *map = mmap (NULL, map_size, PROT_READ, MAP_SHARED, map_fd, 0);
    if (map == MAP_FAILED)
    {
        LOG (stderr, "ERR: mmap failed\n");
        return NULL;
    }

    return map;
}