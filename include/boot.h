#ifndef BOOT_H
#define BOOT_H

#include "kvm_core.h"
#include <stdint.h>

/*
 * Linux x86 boot protocol structures.
 * See Documentation/x86/boot.rst in the kernel tree.
 */

#define BOOT_MAGIC          0xAA55
#define HDR_MAGIC           0x53726448  /* "HdrS" */
#define LINUX_KERNEL_LOAD_ADDR  0x100000    /* 1 MiB — high memory load */
#define BZIMAGE_LOAD_ADDR       0x100000
#define BOOT_PARAMS_ADDR        0x10000     /* guest physical */
#define CMDLINE_ADDR            0x20000
#define STACK_ADDR              0x8000

/* e820 types */
#define E820_RAM        1
#define E820_RESERVED   2

/* Segment selectors for long mode */
#define SEG_NULL    0x00
#define SEG_CODE    0x10
#define SEG_DATA    0x18

/* GDT/IDT base in guest physical memory */
#define GDT_ADDR    0x500
#define IDT_ADDR    0x520
#define PML4_ADDR   0x9000
#define PDPT_ADDR   0xa000
#define PD_ADDR     0xb000

/* MSR indices */
#define MSR_EFER    0xC0000080
#define EFER_LME    (1UL << 8)
#define EFER_LMA    (1UL << 10)

/* CR0 bits */
#define CR0_PE      (1UL << 0)
#define CR0_PG      (1UL << 31)
#define CR0_WP      (1UL << 16)
#define CR0_MP      (1UL << 1)
#define CR0_ET      (1UL << 4)
#define CR0_NE      (1UL << 5)

/* CR4 bits */
#define CR4_PAE     (1UL << 5)
#define CR4_OSFXSR  (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)

/* boot_params / zero_page layout */
#define E820_MAX_ENTRIES 128

struct e820_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} __attribute__((packed));

struct setup_header {
    uint8_t  setup_sects;           /* 0x1f1 */
    uint16_t root_flags;
    uint32_t syssize;
    uint16_t ram_size;
    uint16_t vid_mode;
    uint16_t root_dev;
    uint16_t boot_flag;             /* 0x1fe: 0xAA55 */
    uint16_t jump;
    uint32_t header;                /* "HdrS" */
    uint16_t version;
    uint32_t realmode_swtch;
    uint16_t start_sys_seg;
    uint16_t kernel_version;
    uint8_t  type_of_loader;
    uint8_t  loadflags;
    uint16_t setup_move_size;
    uint32_t code32_start;
    uint32_t ramdisk_image;
    uint32_t ramdisk_size;
    uint32_t bootsect_kludge;
    uint16_t heap_end_ptr;
    uint8_t  ext_loader_ver;
    uint8_t  ext_loader_type;
    uint32_t cmd_line_ptr;
    uint32_t initrd_addr_max;
    uint32_t kernel_alignment;
    uint8_t  relocatable_kernel;
    uint8_t  min_alignment;
    uint16_t xloadflags;
    uint32_t cmdline_size;
    uint32_t hardware_subarch;
    uint64_t hardware_subarch_data;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint64_t setup_data;
    uint64_t pref_address;
    uint32_t init_size;
    uint32_t handover_offset;
} __attribute__((packed));

struct boot_params {
    uint8_t            screen_info[64];       /* 0x000 */
    uint8_t            apm_bios_info[20];     /* 0x040 */
    uint8_t            _pad2[4];              /* 0x054 */
    uint64_t           tboot_addr;            /* 0x058 */
    uint8_t            ist_info[16];          /* 0x060 */
    uint8_t            _pad3[16];             /* 0x070 */
    uint8_t            hd0_info[16];          /* 0x080 */
    uint8_t            hd1_info[16];          /* 0x090 */
    uint8_t            sys_desc_table[16];    /* 0x0a0 */
    uint8_t            olpc_ofw_header[16];   /* 0x0b0 */
    uint32_t           ext_ramdisk_image;     /* 0x0c0 */
    uint32_t           ext_ramdisk_size;      /* 0x0c4 */
    uint32_t           ext_cmd_line_ptr;      /* 0x0c8 */
    uint8_t            _pad4[116];            /* 0x0cc */
    uint8_t            edid_info[128];        /* 0x140 */
    uint8_t            efi_info[32];          /* 0x1c0 */
    uint32_t           alt_mem_k;             /* 0x1e0 */
    uint32_t           scratch;               /* 0x1e4 */
    uint8_t            e820_entries;          /* 0x1e8 */
    uint8_t            eddbuf_entries;        /* 0x1e9 */
    uint8_t            edd_mbr_sig_buf_entries; /* 0x1ea */
    uint8_t            kbd_status;            /* 0x1eb */
    uint8_t            secure_boot;           /* 0x1ec */
    uint8_t            _pad5[2];              /* 0x1ed */
    uint8_t            sentinel;              /* 0x1ef */
    uint8_t            _pad6[1];              /* 0x1f0 */
    struct setup_header hdr;                  /* 0x1f1 */
    uint8_t            _pad7[0x290 - 0x1f1 - sizeof(struct setup_header)];
    uint32_t           edd_mbr_sig_buffer[16]; /* 0x290 */
    struct e820_entry  e820_table[E820_MAX_ENTRIES]; /* 0x2d0 */
    uint8_t            _pad8[48];             /* 0xcd0 */
    uint8_t            eddbuf[492];           /* 0xd00 */
    uint8_t            _pad9[276];            /* 0xeec */
} __attribute__((packed));

/* API */
int load_bzimage(vm_t *vm, const char *path, uint64_t *entry_point);
int setup_boot_params(vm_t *vm, const char *cmdline,
                      uint64_t initrd_start, uint64_t initrd_size);
int setup_long_mode(vcpu_t *vcpu, uint64_t entry_point);

#endif /* BOOT_H */
