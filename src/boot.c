/*
 * boot.c — Linux x86-64 boot protocol implementation
 *
 * Loads a bzImage kernel into guest RAM and sets up the vCPU for
 * 64-bit long mode entry per the Linux boot protocol.
 */
#include "boot.h"
#include "kvm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

/* ------------------------------------------------------------------ */
/* GDT helpers                                                          */
/* ------------------------------------------------------------------ */

/* Build a flat 64-bit GDT descriptor */
static uint64_t gdt_entry(uint32_t base, uint32_t limit,
                           uint8_t type, uint8_t dpl, int is64)
{
    uint64_t desc = 0;
    desc |= (uint64_t)(limit & 0xffff);
    desc |= (uint64_t)(base  & 0xffff)  << 16;
    desc |= (uint64_t)((base >> 16) & 0xff) << 32;
    desc |= (uint64_t)type           << 40;
    desc |= (uint64_t)dpl            << 45;
    desc |= (uint64_t)1              << 47; /* P=1 present */
    desc |= (uint64_t)((limit >> 16) & 0xf) << 48;
    desc |= (uint64_t)(is64 ? 1 : 0) << 53; /* L=1 for 64-bit code */
    desc |= (uint64_t)((base >> 24) & 0xff) << 56;
    return desc;
}

static void kvm_seg_from_gdt(struct kvm_segment *seg, uint64_t entry,
                              uint16_t selector)
{
    seg->base     = ((entry >> 16) & 0xffffff) | (((entry >> 56) & 0xff) << 24);
    seg->limit    = (entry & 0xffff) | (((entry >> 48) & 0xf) << 16);
    seg->selector = selector;
    seg->type     = (entry >> 40) & 0xf;
    seg->present  = (entry >> 47) & 1;
    seg->dpl      = (entry >> 45) & 3;
    seg->db       = (entry >> 54) & 1;
    seg->s        = (entry >> 44) & 1;
    seg->l        = (entry >> 53) & 1;
    seg->g        = (entry >> 55) & 1;
    seg->avl      = (entry >> 52) & 1;
    seg->unusable = !seg->present;
}

/* ------------------------------------------------------------------ */
/* Long mode setup                                                      */
/* ------------------------------------------------------------------ */

/*
 * setup_long_mode — configure vCPU registers for 64-bit long mode entry.
 *
 * Memory layout (in guest physical):
 *   0x9000 — PML4 table (one entry)
 *   0xa000 — PDPT   (one entry, maps 0..1GB)
 *   0xb000 — PD     (512 entries, 2MB pages, identity maps 0..1GB)
 *   0x500  — GDT    (3 entries: null, code64, data64)
 */
int setup_long_mode(vcpu_t *vcpu, uint64_t entry_point)
{
    vm_t *vm = vcpu->vm;

    /* --- Page tables --- */
    uint64_t *pml4 = gpa_to_hva(vm, PML4_ADDR);
    uint64_t *pdpt = gpa_to_hva(vm, PDPT_ADDR);
    uint64_t *pd   = gpa_to_hva(vm, PD_ADDR);

    if (!pml4 || !pdpt || !pd) {
        fprintf(stderr, "[boot] page table area not in RAM\n");
        return -1;
    }

    memset(pml4, 0, 0x1000);
    memset(pdpt, 0, 0x1000);
    memset(pd,   0, 0x1000);

    /* PML4[0] → PDPT */
    pml4[0] = PDPT_ADDR | 3;  /* present + writable */

    /* PDPT[0] → PD (maps 0..1GB) */
    pdpt[0] = PD_ADDR | 3;

    /* PD: 512 × 2MB pages, identity mapped */
    for (int i = 0; i < 512; i++) {
        pd[i] = ((uint64_t)i << 21) | 0x83; /* present + writable + PS */
    }

    /* --- GDT --- */
    uint64_t *gdt = gpa_to_hva(vm, GDT_ADDR);
    if (!gdt) {
        fprintf(stderr, "[boot] GDT area not in RAM\n");
        return -1;
    }

    /* 0x00: null descriptor */
    gdt[0] = 0;
    /* 0x08 → selector 0x08: placeholder, Linux uses 0x10 for code */
    gdt[1] = 0;
    /* 0x10 → selector 0x10: 64-bit code, DPL0, type=0x9a (code, readable, accessed) */
    gdt[2] = gdt_entry(0, 0xfffff, 0x9a, 0, 1);
    /* 0x18 → selector 0x18: 64-bit data, DPL0, type=0x92 (data, writable, accessed) */
    gdt[3] = gdt_entry(0, 0xfffff, 0x92, 0, 0);

    /* --- Set up sregs --- */
    struct kvm_sregs sregs;
    if (vcpu_get_sregs(vcpu, &sregs) < 0) return -1;

    /* CR3 → PML4 */
    sregs.cr3 = PML4_ADDR;

    /* CR4: enable PAE */
    sregs.cr4 = CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT;

    /* CR0: enable protected mode + paging */
    sregs.cr0 = CR0_PE | CR0_MP | CR0_ET | CR0_NE | CR0_WP | CR0_PG;

    /* EFER: LME + LMA */
    sregs.efer = EFER_LME | EFER_LMA;

    /* GDT */
    sregs.gdt.base  = GDT_ADDR;
    sregs.gdt.limit = 4 * 8 - 1;  /* 4 entries */

    /* IDT (null) */
    sregs.idt.base  = IDT_ADDR;
    sregs.idt.limit = 0;

    /* Segments — flat 64-bit */
    kvm_seg_from_gdt(&sregs.cs, gdt[2], 0x10);
    kvm_seg_from_gdt(&sregs.ds, gdt[3], 0x18);
    kvm_seg_from_gdt(&sregs.es, gdt[3], 0x18);
    kvm_seg_from_gdt(&sregs.fs, gdt[3], 0x18);
    kvm_seg_from_gdt(&sregs.gs, gdt[3], 0x18);
    kvm_seg_from_gdt(&sregs.ss, gdt[3], 0x18);

    if (vcpu_set_sregs(vcpu, &sregs) < 0) return -1;

    /* --- Set up regs --- */
    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));

    regs.rip    = entry_point;
    regs.rsp    = STACK_ADDR;  /* small stack under 64K */
    regs.rflags = 0x2;         /* reserved bit always 1 */

    /* Linux boot protocol: rsi = pointer to boot_params */
    regs.rsi    = BOOT_PARAMS_ADDR;

    if (vcpu_set_regs(vcpu, &regs) < 0) return -1;

    printf("[boot] long mode set: entry=0x%llx, cr3=0x%x\n",
           (unsigned long long)entry_point, PML4_ADDR);
    return 0;
}

/* ------------------------------------------------------------------ */
/* bzImage loader                                                       */
/* ------------------------------------------------------------------ */

int load_bzimage(vm_t *vm, const char *path, uint64_t *entry_point)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    /* Read setup header to find number of setup sectors */
    uint8_t buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 1024) {
        fprintf(stderr, "[boot] too small to be a bzImage\n");
        close(fd);
        return -1;
    }

    struct setup_header *hdr = (struct setup_header *)(buf + 0x1f1);

    if (hdr->header != HDR_MAGIC) {
        fprintf(stderr, "[boot] bad header magic: 0x%x (expected 0x%x)\n",
                hdr->header, HDR_MAGIC);
        close(fd);
        return -1;
    }

    uint8_t setup_sects = hdr->setup_sects;
    if (setup_sects == 0) setup_sects = 4;
    uint32_t setup_size = (setup_sects + 1) * 512;

    printf("[boot] bzImage: setup_sects=%u, setup_size=%u, version=0x%x\n",
           setup_sects, setup_size, hdr->version);

    /* The protected-mode kernel starts at byte offset setup_size */
    struct stat st;
    fstat(fd, &st);
    uint64_t kernel_size = st.st_size - setup_size;

    printf("[boot] kernel size: %lu bytes, loading at 0x%x\n",
           (unsigned long)kernel_size, BZIMAGE_LOAD_ADDR);

    /* Read the full bzImage */
    uint8_t *image = malloc(st.st_size);
    if (!image) {
        perror("malloc bzImage");
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    ssize_t total = 0;
    while (total < st.st_size) {
        ssize_t r = read(fd, image + total, st.st_size - total);
        if (r <= 0) break;
        total += r;
    }
    close(fd);

    /* Copy protected-mode kernel to guest RAM at 1MiB */
    uint8_t *dst = gpa_to_hva(vm, BZIMAGE_LOAD_ADDR);
    if (!dst) {
        fprintf(stderr, "[boot] 0x%x not in guest RAM\n", BZIMAGE_LOAD_ADDR);
        free(image);
        return -1;
    }
    memcpy(dst, image + setup_size, kernel_size);

    /* Copy boot_params (the first 4K of image = zero_page) to 0x10000 */
    uint8_t *bp_dst = gpa_to_hva(vm, BOOT_PARAMS_ADDR);
    if (!bp_dst) {
        fprintf(stderr, "[boot] boot_params area not in RAM\n");
        free(image);
        return -1;
    }
    memcpy(bp_dst, image, sizeof(struct boot_params));

    free(image);

    /* Entry point: Linux 64-bit boot protocol jumps to load_addr+0x200 */
    *entry_point = BZIMAGE_LOAD_ADDR + 0x200;
    printf("[boot] entry point: 0x%llx\n", (unsigned long long)*entry_point);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Boot params setup                                                    */
/* ------------------------------------------------------------------ */

int setup_boot_params(vm_t *vm, const char *cmdline,
                      uint64_t initrd_start, uint64_t initrd_size)
{
    struct boot_params *bp = gpa_to_hva(vm, BOOT_PARAMS_ADDR);
    if (!bp) {
        fprintf(stderr, "[boot] boot_params not in RAM\n");
        return -1;
    }

    struct setup_header *hdr = &bp->hdr;

    /* Type of loader: 0xff = undefined */
    hdr->type_of_loader = 0xff;

    /* Command line */
    if (cmdline && *cmdline) {
        char *cl_dst = gpa_to_hva(vm, CMDLINE_ADDR);
        if (cl_dst) {
            strncpy(cl_dst, cmdline, 4096 - 1);
            hdr->cmd_line_ptr  = CMDLINE_ADDR;
            hdr->cmdline_size  = (uint32_t)strlen(cmdline);
        }
    }

    /* Initrd */
    if (initrd_start && initrd_size) {
        hdr->ramdisk_image = (uint32_t)initrd_start;
        hdr->ramdisk_size  = (uint32_t)initrd_size;
    } else {
        hdr->ramdisk_image = 0;
        hdr->ramdisk_size  = 0;
    }

    /* loadflags: bit 0 = loaded high (required for bzImage), bit 5 = quiet */
    hdr->loadflags = 0x01;  /* LOADED_HIGH */

    /* e820 memory map */
    /* Entry 0: 0..640K RAM */
    bp->e820_table[0].addr = 0x0;
    bp->e820_table[0].size = 0x9fc00;
    bp->e820_table[0].type = E820_RAM;

    /* Entry 1: 640K..1MiB reserved (VGA/BIOS) */
    bp->e820_table[1].addr = 0x9fc00;
    bp->e820_table[1].size = 0x400;
    bp->e820_table[1].type = E820_RESERVED;

    /* Entry 2: 1MiB..RAM_SIZE RAM */
    bp->e820_table[2].addr = 0x100000;
    bp->e820_table[2].size = vm->ram_size - 0x100000;
    bp->e820_table[2].type = E820_RAM;

    bp->e820_entries = 3;

    printf("[boot] boot_params configured, cmdline='%s'\n",
           cmdline ? cmdline : "(none)");
    return 0;
}
