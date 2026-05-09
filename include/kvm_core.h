#ifndef KVM_CORE_H
#define KVM_CORE_H

#include <linux/kvm.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_VCPUS 16
#define MAX_MEM_SLOTS 8

/* Forward declarations */
struct vm;
struct vcpu;

/* KVM handle */
typedef struct kvm_handle {
    int fd;
    int api_version;
    int vcpu_mmap_size;
} kvm_handle_t;

/* Memory slot */
typedef struct mem_slot {
    uint32_t slot;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    void    *userspace_addr;  /* mmap'd host pointer */
} mem_slot_t;

/* VM structure */
typedef struct vm {
    int             fd;
    kvm_handle_t   *kvm;
    int             id;

    mem_slot_t      mem_slots[MAX_MEM_SLOTS];
    int             num_mem_slots;

    struct vcpu    *vcpus[MAX_VCPUS];
    int             num_vcpus;

    /* Primary guest RAM (slot 0) */
    void           *ram;
    uint64_t        ram_size;
} vm_t;

/* vCPU structure */
typedef struct vcpu {
    int              fd;
    int              id;
    vm_t            *vm;
    struct kvm_run  *kvm_run;  /* mmap'd kvm_run region */
    int              mmap_size;

    /* Halt flag */
    volatile int     halted;
    volatile int     stop;
} vcpu_t;

/* API */
kvm_handle_t *kvm_init(void);
void          kvm_destroy(kvm_handle_t *kvm);

vm_t   *vm_create(kvm_handle_t *kvm);
void    vm_destroy(vm_t *vm);
int     vm_set_memory(vm_t *vm, uint64_t size_mb);
int     vm_add_mem_slot(vm_t *vm, uint32_t slot,
                         uint64_t guest_phys, uint64_t size, void *host_ptr);

vcpu_t *vcpu_create(vm_t *vm, int id);
void    vcpu_destroy(vcpu_t *vcpu);

int     vcpu_get_regs(vcpu_t *vcpu, struct kvm_regs *regs);
int     vcpu_set_regs(vcpu_t *vcpu, const struct kvm_regs *regs);
int     vcpu_get_sregs(vcpu_t *vcpu, struct kvm_sregs *sregs);
int     vcpu_set_sregs(vcpu_t *vcpu, const struct kvm_sregs *sregs);
int     vcpu_get_fpu(vcpu_t *vcpu, struct kvm_fpu *fpu);
int     vcpu_set_fpu(vcpu_t *vcpu, const struct kvm_fpu *fpu);
int     vcpu_run(vcpu_t *vcpu);

/* Translate guest physical to host virtual */
void   *gpa_to_hva(vm_t *vm, uint64_t gpa);

#endif /* KVM_CORE_H */
