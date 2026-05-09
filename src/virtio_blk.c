/*
 * virtio_blk.c — Virtio-MMIO block device emulation
 *
 * Implements virtio-blk spec v1.x over MMIO at VIRTIO_BLK_MMIO_BASE.
 * Handles KVM_EXIT_MMIO reads/writes. Processes virtqueue I/O requests
 * synchronously (no separate thread needed for simplicity).
 */
#include "virtio_blk.h"
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
/* Device creation / destruction                                        */
/* ------------------------------------------------------------------ */

virtio_blk_t *virtio_blk_create(vm_t *vm, const char *disk_path)
{
    virtio_blk_t *dev = calloc(1, sizeof(*dev));
    if (!dev) { perror("calloc virtio_blk"); return NULL; }

    dev->vm = vm;

    dev->disk_fd = open(disk_path, O_RDWR);
    if (dev->disk_fd < 0) {
        /* Try read-only */
        dev->disk_fd = open(disk_path, O_RDONLY);
        if (dev->disk_fd < 0) {
            perror(disk_path);
            free(dev);
            return NULL;
        }
        printf("[virtio-blk] opened %s read-only\n", disk_path);
    } else {
        printf("[virtio-blk] opened %s read-write\n", disk_path);
    }

    struct stat st;
    if (fstat(dev->disk_fd, &st) < 0) {
        perror("fstat disk");
        close(dev->disk_fd);
        free(dev);
        return NULL;
    }
    dev->disk_size = (uint64_t)st.st_size;

    /* Fill virtio-blk config */
    dev->config.capacity = dev->disk_size / 512;
    dev->config.blk_size = 512;
    dev->config.size_max = 65536;
    dev->config.seg_max  = 128;

    /* Queue defaults */
    dev->queue_num = VIRTQ_SIZE;

    printf("[virtio-blk] disk size: %llu MB (%llu sectors)\n",
           (unsigned long long)(dev->disk_size / (1024*1024)),
           (unsigned long long)dev->config.capacity);

    return dev;
}

void virtio_blk_destroy(virtio_blk_t *dev)
{
    if (!dev) return;
    if (dev->disk_fd >= 0) close(dev->disk_fd);
    free(dev);
}

/* ------------------------------------------------------------------ */
/* Virtqueue processing                                                 */
/* ------------------------------------------------------------------ */

static void process_virtqueue(virtio_blk_t *dev)
{
    vm_t *vm = dev->vm;

    if (!dev->queue_ready || !dev->queue_desc || !dev->queue_avail || !dev->queue_used)
        return;

    struct virtq_avail *avail = gpa_to_hva(vm, dev->queue_avail);
    struct virtq_used  *used  = gpa_to_hva(vm, dev->queue_used);
    struct virtq_desc  *descs = gpa_to_hva(vm, dev->queue_desc);

    if (!avail || !used || !descs) {
        fprintf(stderr, "[virtio-blk] virtqueue pointers not in RAM\n");
        return;
    }

    while (dev->last_avail_idx != avail->idx) {
        uint16_t avail_idx = dev->last_avail_idx % dev->queue_num;
        uint16_t desc_idx  = avail->ring[avail_idx];
        dev->last_avail_idx++;

        /* Walk the descriptor chain */
        /* Descriptor 0: request header */
        struct virtq_desc *d = &descs[desc_idx];
        struct virtio_blk_req_hdr *req_hdr = gpa_to_hva(vm, d->addr);
        if (!req_hdr) {
            fprintf(stderr, "[virtio-blk] req_hdr not in RAM\n");
            continue;
        }

        uint32_t type   = req_hdr->type;
        uint64_t sector = req_hdr->sector;
        uint32_t total_bytes = 0;
        uint8_t  status = VIRTIO_BLK_S_OK;

        /* Move to data descriptor(s) */
        if (!(d->flags & VIRTQ_DESC_F_NEXT)) {
            fprintf(stderr, "[virtio-blk] no data descriptor\n");
            continue;
        }
        d = &descs[d->next];

        /* Process all data descriptors */
        while (1) {
            void *buf = gpa_to_hva(vm, d->addr);
            if (!buf) {
                fprintf(stderr, "[virtio-blk] data buf not in RAM\n");
                status = VIRTIO_BLK_S_IOERR;
                break;
            }

            /* Last descriptor is status byte — skip it here */
            if (!(d->flags & VIRTQ_DESC_F_NEXT)) break;

            uint64_t offset = sector * 512;
            uint32_t len    = d->len;

            if (type == VIRTIO_BLK_T_IN) {
                /* Read from disk */
                ssize_t r = pread(dev->disk_fd, buf, len, (off_t)offset);
                if (r < 0) {
                    perror("[virtio-blk] pread");
                    status = VIRTIO_BLK_S_IOERR;
                } else if ((uint32_t)r < len) {
                    memset((uint8_t *)buf + r, 0, len - (uint32_t)r);
                }
                total_bytes += len;
                sector += len / 512;
            } else if (type == VIRTIO_BLK_T_OUT) {
                /* Write to disk */
                ssize_t r = pwrite(dev->disk_fd, buf, len, (off_t)offset);
                if (r < 0) {
                    perror("[virtio-blk] pwrite");
                    status = VIRTIO_BLK_S_IOERR;
                }
                total_bytes += len;
                sector += len / 512;
            } else if (type == VIRTIO_BLK_T_FLUSH) {
                fsync(dev->disk_fd);
            } else {
                status = VIRTIO_BLK_S_UNSUPP;
            }

            d = &descs[d->next];
        }

        /* Write status byte into the last (write-only) descriptor */
        uint8_t *status_p = gpa_to_hva(vm, d->addr);
        if (status_p) *status_p = status;

        /* Add to used ring */
        uint16_t used_idx = used->idx % dev->queue_num;
        used->ring[used_idx].id  = desc_idx;
        used->ring[used_idx].len = total_bytes + 1; /* +1 for status */
        __sync_synchronize();
        used->idx++;

        dev->interrupt_status |= 1;  /* used buffer notification */
    }
}

/* ------------------------------------------------------------------ */
/* MMIO register read/write                                             */
/* ------------------------------------------------------------------ */

static uint32_t mmio_read32(virtio_blk_t *dev, uint64_t offset)
{
    if (offset >= VIRTIO_MMIO_CONFIG &&
        offset < VIRTIO_MMIO_CONFIG + sizeof(dev->config)) {
        /* Config space */
        uint32_t val = 0;
        memcpy(&val,
               (uint8_t *)&dev->config + (offset - VIRTIO_MMIO_CONFIG),
               4);
        return val;
    }

    switch (offset) {
    case VIRTIO_MMIO_MAGIC:           return VIRTIO_MMIO_MAGIC_VALUE;
    case VIRTIO_MMIO_VERSION_REG:     return VIRTIO_MMIO_VERSION;
    case VIRTIO_MMIO_DEVICE_ID:       return VIRTIO_ID_BLOCK;
    case VIRTIO_MMIO_VENDOR_ID:       return 0x554d4551; /* "QEMU" */
    case VIRTIO_MMIO_DEVICE_FEATURES:
        if (dev->device_features_sel == 0)
            return (1u << VIRTIO_BLK_F_SEG_MAX) |
                   (1u << VIRTIO_BLK_F_SIZE_MAX);
        else if (dev->device_features_sel == 1)
            return (1u << (VIRTIO_F_VERSION_1 - 32));
        return 0;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:   return VIRTQ_SIZE;
    case VIRTIO_MMIO_QUEUE_READY:     return dev->queue_ready;
    case VIRTIO_MMIO_INTERRUPT_STATUS: return dev->interrupt_status;
    case VIRTIO_MMIO_STATUS:          return dev->status;
    case VIRTIO_MMIO_CONFIG_GEN:      return 0;
    default:
        return 0;
    }
}

static void mmio_write32(virtio_blk_t *dev, uint64_t offset, uint32_t val)
{
    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        dev->device_features_sel = val; break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        dev->driver_features = val; break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        dev->driver_features_sel = val; break;
    case VIRTIO_MMIO_QUEUE_SEL:
        dev->queue_sel = val; break;
    case VIRTIO_MMIO_QUEUE_NUM:
        if (val <= VIRTQ_SIZE) dev->queue_num = val;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        dev->queue_ready = val; break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        process_virtqueue(dev); break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        dev->interrupt_status &= ~val; break;
    case VIRTIO_MMIO_STATUS:
        dev->status = val; break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        dev->queue_desc = (dev->queue_desc & 0xffffffff00000000ULL) | val; break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        dev->queue_desc = (dev->queue_desc & 0x00000000ffffffffULL) | ((uint64_t)val << 32); break;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
        dev->queue_avail = (dev->queue_avail & 0xffffffff00000000ULL) | val; break;
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        dev->queue_avail = (dev->queue_avail & 0x00000000ffffffffULL) | ((uint64_t)val << 32); break;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
        dev->queue_used = (dev->queue_used & 0xffffffff00000000ULL) | val; break;
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        dev->queue_used = (dev->queue_used & 0x00000000ffffffffULL) | ((uint64_t)val << 32); break;
    default:
        break;
    }
}

/*
 * Handle a KVM_EXIT_MMIO event.
 * Returns 1 if it was our MMIO range, 0 otherwise.
 */
int virtio_blk_handle_mmio(virtio_blk_t *dev, struct kvm_run *run)
{
    uint64_t addr = run->mmio.phys_addr;

    if (addr < VIRTIO_BLK_MMIO_BASE ||
        addr >= VIRTIO_BLK_MMIO_BASE + VIRTIO_BLK_MMIO_SIZE)
        return 0;

    uint64_t offset = addr - VIRTIO_BLK_MMIO_BASE;
    int is_write = run->mmio.is_write;
    uint32_t len = run->mmio.len;
    uint8_t *data = run->mmio.data;

    if (len == 4) {
        uint32_t val = 0;
        if (!is_write) {
            val = mmio_read32(dev, offset);
            memcpy(data, &val, 4);
        } else {
            memcpy(&val, data, 4);
            mmio_write32(dev, offset, val);
        }
    } else if (len == 1 && !is_write &&
               offset >= VIRTIO_MMIO_CONFIG &&
               offset < VIRTIO_MMIO_CONFIG + sizeof(dev->config)) {
        /* Byte access into config space */
        data[0] = ((uint8_t *)&dev->config)[offset - VIRTIO_MMIO_CONFIG];
    }

    return 1;
}
