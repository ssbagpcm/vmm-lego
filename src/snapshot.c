/*
 * snapshot.c — Hot VM snapshot: save/restore vCPU state and guest RAM
 */
#include "snapshot.h"
#include "kvm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

/* Helper: write exactly N bytes, ignore return (we handle errors via perror) */
static void xwrite(int fd, const void *buf, size_t n)
{
    ssize_t r = write(fd, buf, n);
    if (r < 0) perror("snapshot write");
}

/* MSR indices we save/restore */
static uint32_t msr_list[] = {
    0xC0000080,  /* EFER */
    0xC0000081,  /* STAR */
    0xC0000082,  /* LSTAR */
    0xC0000083,  /* CSTAR */
    0xC0000100,  /* FS_BASE */
    0xC0000101,  /* GS_BASE */
    0xC0000102,  /* KERNEL_GS_BASE */
};
#define NUM_MSRS (sizeof(msr_list)/sizeof(msr_list[0]))

/* Pause/resume by setting the stop flag on all vCPUs */
void vm_pause(vm_t *vm)
{
    for (int i = 0; i < vm->num_vcpus; i++) {
        if (vm->vcpus[i]) {
            vm->vcpus[i]->stop = 1;
        }
    }
    /* Give vCPUs a moment to exit KVM_RUN */
    usleep(10000);
}

void vm_resume(vm_t *vm)
{
    for (int i = 0; i < vm->num_vcpus; i++) {
        if (vm->vcpus[i]) {
            vm->vcpus[i]->stop = 0;
        }
    }
}

/* Save one vCPU's state */
static int save_vcpu(vcpu_t *vcpu, snapshot_vcpu_state_t *state)
{
    state->vcpu_id = vcpu->id;

    if (vcpu_get_regs(vcpu,  &state->regs)  < 0) return -1;
    if (vcpu_get_sregs(vcpu, &state->sregs) < 0) return -1;
    if (vcpu_get_fpu(vcpu,   &state->fpu)   < 0) return -1;

    /* MSRs */
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[NUM_MSRS];
    } msrs;

    memset(&msrs, 0, sizeof(msrs));
    msrs.header.nmsrs = NUM_MSRS;
    for (size_t i = 0; i < NUM_MSRS; i++)
        msrs.header.entries[i].index = msr_list[i];

    if (ioctl(vcpu->fd, KVM_GET_MSRS, &msrs.header) < 0) {
        perror("KVM_GET_MSRS");
        /* Non-fatal: continue without MSRs */
    } else {
        state->msr_efer           = msrs.header.entries[0].data;
        state->msr_star           = msrs.header.entries[1].data;
        state->msr_lstar          = msrs.header.entries[2].data;
        state->msr_cstar          = msrs.header.entries[3].data;
        state->msr_fs_base        = msrs.header.entries[4].data;
        state->msr_gs_base        = msrs.header.entries[5].data;
        state->msr_kernel_gs_base = msrs.header.entries[6].data;
    }

    return 0;
}

/* Restore one vCPU's state */
static int restore_vcpu(vcpu_t *vcpu, const snapshot_vcpu_state_t *state)
{
    if (vcpu_set_regs(vcpu,  &state->regs)  < 0) return -1;
    if (vcpu_set_sregs(vcpu, &state->sregs) < 0) return -1;
    if (vcpu_set_fpu(vcpu,   &state->fpu)   < 0) return -1;

    /* Restore MSRs */
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[NUM_MSRS];
    } msrs;

    memset(&msrs, 0, sizeof(msrs));
    msrs.header.nmsrs = NUM_MSRS;
    msrs.header.entries[0] = (struct kvm_msr_entry){ .index = msr_list[0], .data = state->msr_efer };
    msrs.header.entries[1] = (struct kvm_msr_entry){ .index = msr_list[1], .data = state->msr_star };
    msrs.header.entries[2] = (struct kvm_msr_entry){ .index = msr_list[2], .data = state->msr_lstar };
    msrs.header.entries[3] = (struct kvm_msr_entry){ .index = msr_list[3], .data = state->msr_cstar };
    msrs.header.entries[4] = (struct kvm_msr_entry){ .index = msr_list[4], .data = state->msr_fs_base };
    msrs.header.entries[5] = (struct kvm_msr_entry){ .index = msr_list[5], .data = state->msr_gs_base };
    msrs.header.entries[6] = (struct kvm_msr_entry){ .index = msr_list[6], .data = state->msr_kernel_gs_base };

    if (ioctl(vcpu->fd, KVM_SET_MSRS, &msrs.header) < 0) {
        perror("KVM_SET_MSRS");
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* snapshot_save                                                        */
/* ------------------------------------------------------------------ */

int snapshot_save(vm_t *vm, const char *path,
                  serial_dev_t *serial, virtio_blk_t *blk)
{
    (void)blk;  /* virtio state is stateless enough for now */

    printf("[snapshot] saving to %s ...\n", path);
    vm_pause(vm);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(path);
        vm_resume(vm);
        return -1;
    }

    /* Write header */
    snapshot_header_t hdr = {
        .magic     = SNAPSHOT_MAGIC,
        .version   = SNAPSHOT_VERSION,
        .num_vcpus = (uint32_t)vm->num_vcpus,
        .ram_size  = vm->ram_size,
        .timestamp = (uint64_t)time(NULL),
    };
    xwrite(fd, &hdr, sizeof(hdr));

    /* Write vCPU states */
    for (int i = 0; i < vm->num_vcpus; i++) {
        snapshot_vcpu_state_t state;
        memset(&state, 0, sizeof(state));
        save_vcpu(vm->vcpus[i], &state);

        uint32_t section = SNAP_SECTION_VCPU;
        uint64_t size    = sizeof(state);
        xwrite(fd, &section, sizeof(section));
        xwrite(fd, &size,    sizeof(size));
        xwrite(fd, &state,   sizeof(state));
    }

    /* Write RAM */
    {
        uint32_t section = SNAP_SECTION_RAM;
        uint64_t size    = vm->ram_size;
        xwrite(fd, &section, sizeof(section));
        xwrite(fd, &size,    sizeof(size));

        ssize_t written = 0;
        const uint8_t *p = (const uint8_t *)vm->ram;
        while ((uint64_t)written < vm->ram_size) {
            ssize_t n = write(fd, p + written, vm->ram_size - (uint64_t)written);
            if (n <= 0) { perror("write RAM"); break; }
            written += n;
        }
        printf("[snapshot] wrote %zd bytes of RAM\n", written);
    }

    /* Write serial state */
    if (serial) {
        uint32_t section = SNAP_SECTION_SERIAL;
        uint64_t size    = sizeof(*serial);
        xwrite(fd, &section, sizeof(section));
        xwrite(fd, &size,    sizeof(size));
        xwrite(fd, serial,   sizeof(*serial));
    }

    close(fd);
    vm_resume(vm);
    printf("[snapshot] saved successfully\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* snapshot_restore                                                     */
/* ------------------------------------------------------------------ */

int snapshot_restore(vm_t *vm, const char *path,
                     serial_dev_t *serial, virtio_blk_t *blk)
{
    (void)blk;

    printf("[snapshot] restoring from %s ...\n", path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    snapshot_header_t hdr;
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        hdr.magic != SNAPSHOT_MAGIC) {
        fprintf(stderr, "[snapshot] invalid snapshot file\n");
        close(fd);
        return -1;
    }

    printf("[snapshot] version=%u, num_vcpus=%u, ram_size=%llu MB\n",
           hdr.version, hdr.num_vcpus,
           (unsigned long long)(hdr.ram_size / (1024*1024)));

    /* Read sections */
    while (1) {
        uint32_t section;
        uint64_t size;

        if (read(fd, &section, sizeof(section)) != (ssize_t)sizeof(section)) break;
        if (read(fd, &size,    sizeof(size))    != (ssize_t)sizeof(size))    break;

        switch (section) {
        case SNAP_SECTION_VCPU: {
            snapshot_vcpu_state_t state;
            if (read(fd, &state, sizeof(state)) != (ssize_t)sizeof(state)) goto err;
            int id = state.vcpu_id;
            if (id < vm->num_vcpus && vm->vcpus[id]) {
                restore_vcpu(vm->vcpus[id], &state);
                printf("[snapshot] restored vcpu %d\n", id);
            }
            break;
        }
        case SNAP_SECTION_RAM: {
            ssize_t got = 0;
            uint8_t *p = (uint8_t *)vm->ram;
            while ((uint64_t)got < size) {
                ssize_t n = read(fd, p + got, size - (uint64_t)got);
                if (n <= 0) break;
                got += n;
            }
            printf("[snapshot] restored %zd bytes of RAM\n", got);
            break;
        }
        case SNAP_SECTION_SERIAL: {
            if (serial && size <= sizeof(*serial)) {
                ssize_t rr = read(fd, serial, size);
                (void)rr;
                /* Reset FDs */
                serial->input_fd  = STDIN_FILENO;
                serial->output_fd = STDOUT_FILENO;
                printf("[snapshot] restored serial state\n");
            } else {
                lseek(fd, (off_t)size, SEEK_CUR);
            }
            break;
        }
        default:
            lseek(fd, (off_t)size, SEEK_CUR);
            break;
        }
    }

    close(fd);
    printf("[snapshot] restore complete\n");
    return 0;

err:
    fprintf(stderr, "[snapshot] truncated snapshot\n");
    close(fd);
    return -1;
}
