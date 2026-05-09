/*
 * kvm_core.c — KVM VMM core: KVM init, VM/vCPU lifecycle, memory, run loop
 */
#include "kvm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

/* ------------------------------------------------------------------ */
/* KVM global init                                                      */
/* ------------------------------------------------------------------ */

kvm_handle_t *kvm_init(void)
{
    kvm_handle_t *kvm = calloc(1, sizeof(*kvm));
    if (!kvm) {
        perror("calloc kvm_handle");
        return NULL;
    }

    kvm->fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm->fd < 0) {
        perror("open /dev/kvm");
        free(kvm);
        return NULL;
    }

    kvm->api_version = ioctl(kvm->fd, KVM_GET_API_VERSION, 0);
    if (kvm->api_version != 12) {
        fprintf(stderr, "KVM API version %d (expected 12)\n", kvm->api_version);
        close(kvm->fd);
        free(kvm);
        return NULL;
    }
    printf("[kvm] API version: %d\n", kvm->api_version);

    kvm->vcpu_mmap_size = ioctl(kvm->fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (kvm->vcpu_mmap_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        close(kvm->fd);
        free(kvm);
        return NULL;
    }
    printf("[kvm] VCPU mmap size: %d\n", kvm->vcpu_mmap_size);

    return kvm;
}

void kvm_destroy(kvm_handle_t *kvm)
{
    if (!kvm) return;
    if (kvm->fd >= 0) close(kvm->fd);
    free(kvm);
}

/* ------------------------------------------------------------------ */
/* VM lifecycle                                                         */
/* ------------------------------------------------------------------ */

static int next_vm_id = 0;

vm_t *vm_create(kvm_handle_t *kvm)
{
    vm_t *vm = calloc(1, sizeof(*vm));
    if (!vm) {
        perror("calloc vm");
        return NULL;
    }

    vm->kvm = kvm;
    vm->id  = next_vm_id++;

    vm->fd = ioctl(kvm->fd, KVM_CREATE_VM, 0);
    if (vm->fd < 0) {
        perror("KVM_CREATE_VM");
        free(vm);
        return NULL;
    }
    printf("[vm %d] created, fd=%d\n", vm->id, vm->fd);
    return vm;
}

void vm_destroy(vm_t *vm)
{
    if (!vm) return;

    /* Destroy vCPUs */
    for (int i = 0; i < vm->num_vcpus; i++) {
        if (vm->vcpus[i]) vcpu_destroy(vm->vcpus[i]);
    }

    /* Unmap guest RAM */
    for (int i = 0; i < vm->num_mem_slots; i++) {
        if (vm->mem_slots[i].userspace_addr) {
            munmap(vm->mem_slots[i].userspace_addr,
                   vm->mem_slots[i].memory_size);
        }
    }

    if (vm->fd >= 0) close(vm->fd);
    free(vm);
}

/* ------------------------------------------------------------------ */
/* Memory management                                                    */
/* ------------------------------------------------------------------ */

int vm_add_mem_slot(vm_t *vm, uint32_t slot,
                    uint64_t guest_phys, uint64_t size, void *host_ptr)
{
    struct kvm_userspace_memory_region region = {
        .slot            = slot,
        .flags           = 0,
        .guest_phys_addr = guest_phys,
        .memory_size     = size,
        .userspace_addr  = (uint64_t)(uintptr_t)host_ptr,
    };

    if (ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return -1;
    }

    if (vm->num_mem_slots < MAX_MEM_SLOTS) {
        int idx = vm->num_mem_slots++;
        vm->mem_slots[idx].slot            = slot;
        vm->mem_slots[idx].guest_phys_addr = guest_phys;
        vm->mem_slots[idx].memory_size     = size;
        vm->mem_slots[idx].userspace_addr  = host_ptr;
    }

    return 0;
}

int vm_set_memory(vm_t *vm, uint64_t size_mb)
{
    uint64_t size = size_mb * 1024 * 1024;

    /* mmap anonymous memory for guest RAM */
    void *ram = mmap(NULL, size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap guest RAM");
        return -1;
    }
    memset(ram, 0, size);

    vm->ram      = ram;
    vm->ram_size = size;

    printf("[vm %d] guest RAM: %lu MB at host %p\n", vm->id, (unsigned long)size_mb, ram);

    /* Slot 0: below 3 GiB hole — for now map everything */
    if (vm_add_mem_slot(vm, 0, 0, size, ram) < 0) {
        munmap(ram, size);
        vm->ram = NULL;
        return -1;
    }

    return 0;
}

/* Translate guest physical address to host virtual */
void *gpa_to_hva(vm_t *vm, uint64_t gpa)
{
    for (int i = 0; i < vm->num_mem_slots; i++) {
        mem_slot_t *s = &vm->mem_slots[i];
        if (gpa >= s->guest_phys_addr &&
            gpa <  s->guest_phys_addr + s->memory_size) {
            return (uint8_t *)s->userspace_addr + (gpa - s->guest_phys_addr);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* vCPU lifecycle                                                       */
/* ------------------------------------------------------------------ */

vcpu_t *vcpu_create(vm_t *vm, int id)
{
    vcpu_t *vcpu = calloc(1, sizeof(*vcpu));
    if (!vcpu) {
        perror("calloc vcpu");
        return NULL;
    }

    vcpu->id  = id;
    vcpu->vm  = vm;

    vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, (unsigned long)id);
    if (vcpu->fd < 0) {
        perror("KVM_CREATE_VCPU");
        free(vcpu);
        return NULL;
    }

    /* mmap the kvm_run structure */
    int mmap_size = vm->kvm->vcpu_mmap_size;
    vcpu->mmap_size = mmap_size;
    vcpu->kvm_run   = mmap(NULL, mmap_size,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, vcpu->fd, 0);
    if (vcpu->kvm_run == MAP_FAILED) {
        perror("mmap kvm_run");
        close(vcpu->fd);
        free(vcpu);
        return NULL;
    }

    if (vm->num_vcpus < MAX_VCPUS) {
        vm->vcpus[vm->num_vcpus++] = vcpu;
    }

    printf("[vcpu %d] created, fd=%d\n", id, vcpu->fd);
    return vcpu;
}

void vcpu_destroy(vcpu_t *vcpu)
{
    if (!vcpu) return;
    if (vcpu->kvm_run && vcpu->kvm_run != MAP_FAILED)
        munmap(vcpu->kvm_run, vcpu->mmap_size);
    if (vcpu->fd >= 0)
        close(vcpu->fd);
    free(vcpu);
}

/* ------------------------------------------------------------------ */
/* Register accessors                                                   */
/* ------------------------------------------------------------------ */

int vcpu_get_regs(vcpu_t *vcpu, struct kvm_regs *regs)
{
    if (ioctl(vcpu->fd, KVM_GET_REGS, regs) < 0) {
        perror("KVM_GET_REGS");
        return -1;
    }
    return 0;
}

int vcpu_set_regs(vcpu_t *vcpu, const struct kvm_regs *regs)
{
    if (ioctl(vcpu->fd, KVM_SET_REGS, regs) < 0) {
        perror("KVM_SET_REGS");
        return -1;
    }
    return 0;
}

int vcpu_get_sregs(vcpu_t *vcpu, struct kvm_sregs *sregs)
{
    if (ioctl(vcpu->fd, KVM_GET_SREGS, sregs) < 0) {
        perror("KVM_GET_SREGS");
        return -1;
    }
    return 0;
}

int vcpu_set_sregs(vcpu_t *vcpu, const struct kvm_sregs *sregs)
{
    if (ioctl(vcpu->fd, KVM_SET_SREGS, sregs) < 0) {
        perror("KVM_SET_SREGS");
        return -1;
    }
    return 0;
}

int vcpu_get_fpu(vcpu_t *vcpu, struct kvm_fpu *fpu)
{
    if (ioctl(vcpu->fd, KVM_GET_FPU, fpu) < 0) {
        perror("KVM_GET_FPU");
        return -1;
    }
    return 0;
}

int vcpu_set_fpu(vcpu_t *vcpu, const struct kvm_fpu *fpu)
{
    if (ioctl(vcpu->fd, KVM_SET_FPU, fpu) < 0) {
        perror("KVM_SET_FPU");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Run loop                                                             */
/* ------------------------------------------------------------------ */

/*
 * vcpu_run: run the vCPU until exit.
 * Returns the exit reason (positive), or -1 on error.
 * Callers handle specific exit types.
 */
int vcpu_run(vcpu_t *vcpu)
{
    int ret = ioctl(vcpu->fd, KVM_RUN, 0);
    if (ret < 0) {
        if (errno == EINTR) return -EINTR;
        perror("KVM_RUN");
        return -1;
    }
    return (int)vcpu->kvm_run->exit_reason;
}
