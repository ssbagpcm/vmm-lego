#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "kvm_core.h"
#include <stdint.h>
#include <pthread.h>

/* MMIO base address for virtio-blk */
#define VIRTIO_BLK_MMIO_BASE    0xd0000000ULL
#define VIRTIO_BLK_MMIO_SIZE    0x1000

/* Virtio MMIO magic / version */
#define VIRTIO_MMIO_MAGIC_VALUE     0x74726976  /* "virt" */
#define VIRTIO_MMIO_VERSION         2

/* Device ID: block = 2 */
#define VIRTIO_ID_BLOCK     2

/* Virtio MMIO register offsets */
#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION_REG     0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_READY     0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4
#define VIRTIO_MMIO_CONFIG_GEN      0x0fc
#define VIRTIO_MMIO_CONFIG          0x100

/* Virtio block config */
#define VIRTIO_BLK_F_RO         5
#define VIRTIO_BLK_F_SEG_MAX    2
#define VIRTIO_BLK_F_SIZE_MAX   1
#define VIRTIO_F_VERSION_1      32

/* Virtio block request types */
#define VIRTIO_BLK_T_IN         0   /* read */
#define VIRTIO_BLK_T_OUT        1   /* write */
#define VIRTIO_BLK_T_FLUSH      4

/* Virtio block status */
#define VIRTIO_BLK_S_OK         0
#define VIRTIO_BLK_S_IOERR      1
#define VIRTIO_BLK_S_UNSUPP     2

/* Queue size */
#define VIRTQ_SIZE  128

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed));

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct virtio_blk_config {
    uint64_t capacity;      /* in 512-byte sectors */
    uint32_t size_max;
    uint32_t seg_max;
    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectors;
    uint32_t blk_size;
    uint8_t  physical_block_exp;
    uint8_t  alignment_offset;
    uint16_t min_io_size;
    uint32_t opt_io_size;
    uint8_t  writeback;
    uint8_t  unused[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t  write_zeroes_may_unmap;
    uint8_t  unused2[3];
} __attribute__((packed));

typedef struct virtio_blk {
    vm_t   *vm;
    int     disk_fd;
    uint64_t disk_size;   /* bytes */

    /* MMIO state */
    uint32_t status;
    uint32_t device_features_sel;
    uint32_t driver_features;
    uint32_t driver_features_sel;
    uint32_t queue_sel;
    uint32_t interrupt_status;

    /* Single virtqueue (queue 0) */
    uint32_t queue_num;
    uint32_t queue_ready;
    uint64_t queue_desc;
    uint64_t queue_avail;
    uint64_t queue_used;

    uint16_t last_avail_idx;

    struct virtio_blk_config config;
} virtio_blk_t;

virtio_blk_t *virtio_blk_create(vm_t *vm, const char *disk_path);
void          virtio_blk_destroy(virtio_blk_t *dev);
int           virtio_blk_handle_mmio(virtio_blk_t *dev, struct kvm_run *run);

#endif /* VIRTIO_BLK_H */
