#ifndef RESOURCES_H
#define RESOURCES_H

#include <stdint.h>
#include <sys/types.h>

/* cgroup v2 root for vmmd */
#define CGROUP_ROOT "/sys/fs/cgroup/vmmd"

typedef struct vm_resources {
    int     vm_id;
    char    cgroup_path[256];
    int     cpu_quota;    /* microseconds per 100ms period (0 = unlimited) */
    long    mem_max_mb;   /* MB limit (0 = unlimited) */
} vm_resources_t;

int resources_init_cgroup(vm_resources_t *res, int vm_id);
int resources_set_cpu(vm_resources_t *res, int quota_us);
int resources_set_memory(vm_resources_t *res, long mem_mb);
int resources_pin_process(vm_resources_t *res, pid_t pid);
int resources_cleanup(vm_resources_t *res);

#endif /* RESOURCES_H */
