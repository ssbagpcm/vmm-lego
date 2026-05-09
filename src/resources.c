/*
 * resources.c — Per-VM resource control via cgroup v2
 */
#include "resources.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Write a string to a cgroup file */
static int cg_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[resources] open %s: %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    if (n < 0) {
        fprintf(stderr, "[resources] write %s = %s: %s\n", path, val, strerror(errno));
        return -1;
    }
    return 0;
}

int resources_init_cgroup(vm_resources_t *res, int vm_id)
{
    res->vm_id = vm_id;
    snprintf(res->cgroup_path, sizeof(res->cgroup_path),
             CGROUP_ROOT "/vm%d", vm_id);

    /* Create cgroup directory hierarchy */
    /* First ensure root vmmd group exists */
    if (mkdir(CGROUP_ROOT, 0755) < 0 && errno != EEXIST) {
        /* cgroup root may not be writable — try enabling subtree_control */
        fprintf(stderr, "[resources] mkdir %s: %s\n", CGROUP_ROOT, strerror(errno));
        return -1;
    }

    /* Enable memory and cpu controllers at root */
    cg_write("/sys/fs/cgroup/cgroup.subtree_control", "+cpu +memory");

    /* Enable at vmmd level */
    char ctrl_path[300];
    snprintf(ctrl_path, sizeof(ctrl_path), "%s/cgroup.subtree_control", CGROUP_ROOT);
    cg_write(ctrl_path, "+cpu +memory");

    /* Create per-VM cgroup */
    if (mkdir(res->cgroup_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[resources] mkdir %s: %s\n", res->cgroup_path, strerror(errno));
        return -1;
    }

    printf("[resources] cgroup created: %s\n", res->cgroup_path);
    return 0;
}

int resources_set_cpu(vm_resources_t *res, int quota_us)
{
    char path[300], val[64];
    snprintf(path, sizeof(path), "%s/cpu.max", res->cgroup_path);
    if (quota_us <= 0) {
        snprintf(val, sizeof(val), "max 100000");
    } else {
        snprintf(val, sizeof(val), "%d 100000", quota_us);
    }
    printf("[resources] cpu.max = %s\n", val);
    return cg_write(path, val);
}

int resources_set_memory(vm_resources_t *res, long mem_mb)
{
    char path[300], val[64];
    snprintf(path, sizeof(path), "%s/memory.max", res->cgroup_path);
    if (mem_mb <= 0) {
        snprintf(val, sizeof(val), "max");
    } else {
        snprintf(val, sizeof(val), "%ld", mem_mb * 1024 * 1024);
    }
    printf("[resources] memory.max = %s\n", val);
    return cg_write(path, val);
}

int resources_pin_process(vm_resources_t *res, pid_t pid)
{
    char path[300], val[32];
    snprintf(path, sizeof(path), "%s/cgroup.procs", res->cgroup_path);
    snprintf(val, sizeof(val), "%d\n", (int)pid);
    printf("[resources] pinning pid %d to %s\n", (int)pid, res->cgroup_path);
    return cg_write(path, val);
}

int resources_cleanup(vm_resources_t *res)
{
    /* rmdir only works when cgroup is empty */
    if (rmdir(res->cgroup_path) < 0) {
        fprintf(stderr, "[resources] rmdir %s: %s\n",
                res->cgroup_path, strerror(errno));
        return -1;
    }
    return 0;
}
