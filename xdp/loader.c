#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <unistd.h>
#include <sys/resource.h>

static void update_rlimit(void) {
    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    int ret = setrlimit(RLIMIT_MEMLOCK, &r);
    if (ret) {
        perror("setrlimit");
        fprintf(stderr, "ERR: setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY) failed\n");
        fprintf(stderr, "Try with sudo\n");
        exit(EXIT_FAILURE);
    }
}

void usage(char *prog) {
    // <prog> <ifname> <action>
    fprintf(stderr, "Usage: %s <ifname> <action> [extra arguments]\n", prog);
    fprintf(stderr, "action:\n");
    fprintf(stderr, "  start: load, attach, pin and start recording pingpong\n");
    fprintf(stderr, "         requires an extra argument: <num of packets>\n");
    fprintf(stderr, "  remove: remove pinned program\n");
}

static const char *filename = "pingpong.o";
static const char *section = "xdp";
static const char *prog_name = "xdp_main";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong";

void start_pingpong(int ifindex, int num_pkts) {
    struct bpf_object *obj = bpf_object__open_file(filename, NULL);
    if (!obj) {
        fprintf(stderr, "ERR: opening file failed\n");
        perror("bpf_object__open_file");
        return;
    }

    int ret = bpf_object__load(obj);
    if (ret) {
        fprintf(stderr, "ERR: loading file failed\n");
        perror("bpf_object__load");
        return;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name(obj, prog_name);
    if (!prog) {
        fprintf(stderr, "ERR: finding program failed\n");
        return;
    }

    ret = bpf_program__pin(prog, pinpath);
    if (ret) {
        fprintf(stderr, "ERR: pinning program failed\n");
        return;
    }


    ret = bpf_xdp_attach(ifindex, bpf_program__fd(prog), XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_DRV_MODE, 0);
    if (ret) {
        fprintf(stderr, "ERR: attaching program failed\n");
        return;
    }
    printf("Program attached to interface %d\n", ifindex);
}

int remove_pingpong(int ifindex) {
    struct bpf_object *obj = bpf_object__open_file(filename, NULL);
    if (!obj) {
        fprintf(stderr, "ERR: opening file failed\n");
        return EXIT_FAILURE;
    }

    int ret = bpf_object__load(obj);
    if (ret) {
        fprintf(stderr, "ERR: loading file failed\n");
        return EXIT_FAILURE;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name(obj, prog_name);
    if (!prog) {
        fprintf(stderr, "ERR: finding program failed\n");
        return EXIT_FAILURE;
    }

    ret = bpf_program__unpin(prog, pinpath); // ret can be ignored, if unpin fails, it's not a big deal

    ret = bpf_xdp_detach(ifindex, XDP_FLAGS_DRV_MODE, 0);
    if (ret) {
        fprintf(stderr, "ERR: detaching program failed\n");
        return EXIT_FAILURE;
    }

    bpf_program__unload(prog);

    printf("Program detached from interface %d\n", ifindex);

    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    update_rlimit();

    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    char *ifname = argv[1];
    char *action = argv[2];

    int ifindex = if_nametoindex(ifname);
    if (!ifindex) {
        perror("if_nametoindex");
        return EXIT_FAILURE;
    }

    if (strcmp(action, "start") == 0) {
        int num_pkts = atoi(argv[3]);
        start_pingpong(ifindex, num_pkts);
    } else if (strcmp(action, "remove") == 0) {
        remove_pingpong(ifindex);
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}