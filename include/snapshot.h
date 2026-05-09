#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "kvm_core.h"
#include "serial.h"
#include "virtio_blk.h"
#include <stdint.h>

/* Magic number for snapshot files */
#define SNAPSHOT_MAGIC      0x564d4d53  /* "VMMS" */
#define SNAPSHOT_VERSION    1

/* Section types */
#define SNAP_SECTION_VCPU   0x01
#define SNAP_SECTION_RAM    0x02
#define SNAP_SECTION_SERIAL 0x03
#define SNAP_SECTION_VIRTIO 0x04

typedef struct snapshot_vcpu_state {
    int             vcpu_id;
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct kvm_fpu  fpu;
    /* MSRs we care about */
    uint64_t        msr_efer;
    uint64_t        msr_star;
    uint64_t        msr_lstar;
    uint64_t        msr_cstar;
    uint64_t        msr_gs_base;
    uint64_t        msr_fs_base;
    uint64_t        msr_kernel_gs_base;
} snapshot_vcpu_state_t;

typedef struct snapshot_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_vcpus;
    uint64_t ram_size;
    uint64_t timestamp;
} snapshot_header_t;

/* Pause/resume all vcpus */
void vm_pause(vm_t *vm);
void vm_resume(vm_t *vm);

int snapshot_save(vm_t *vm, const char *path,
                  serial_dev_t *serial, virtio_blk_t *blk);
int snapshot_restore(vm_t *vm, const char *path,
                     serial_dev_t *serial, virtio_blk_t *blk);

#endif /* SNAPSHOT_H */
