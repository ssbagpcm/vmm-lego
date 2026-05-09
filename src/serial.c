/*
 * serial.c — 16550 UART emulation for COM1 (0x3f8)
 *
 * Handles KVM_EXIT_IO for I/O port range 0x3f8..0x3ff.
 * Guest serial output goes to stdout; stdin feeds guest serial input.
 */
#include "serial.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>

void serial_init(serial_dev_t *dev, int input_fd, int output_fd)
{
    memset(dev, 0, sizeof(*dev));
    dev->lsr       = LSR_THRE | LSR_TEMT;  /* TX always ready */
    dev->msr       = 0xb0;                  /* CTS, DSR, DCD asserted */
    dev->iir       = 0x01;                  /* no interrupt pending */
    dev->input_fd  = input_fd;
    dev->output_fd = output_fd;

    /* Make input non-blocking */
    if (input_fd >= 0) {
        int flags = fcntl(input_fd, F_GETFL, 0);
        fcntl(input_fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* Poll for a received byte. Returns 1 if byte available in rbr. */
static int serial_rx_poll(serial_dev_t *dev)
{
    if (dev->lsr & LSR_DR) return 1;  /* already have data */
    if (dev->input_fd < 0) return 0;

    uint8_t ch;
    ssize_t r = read(dev->input_fd, &ch, 1);
    if (r == 1) {
        dev->rbr = ch;
        dev->lsr |= LSR_DR;
        return 1;
    }
    return 0;
}

uint8_t serial_read(serial_dev_t *dev, uint16_t port)
{
    uint8_t reg = (uint8_t)(port - SERIAL_PORT_BASE);
    uint8_t val = 0;

    /* DLAB mode: DLL/DLH */
    if (dev->lcr & 0x80) {
        if (reg == SERIAL_DLL) return (uint8_t)(dev->dll & 0xff);
        if (reg == SERIAL_DLH) return (uint8_t)(dev->dll >> 8);
    }

    switch (reg) {
    case SERIAL_RBR:
        serial_rx_poll(dev);
        val = dev->rbr;
        dev->lsr &= ~LSR_DR;  /* clear data ready */
        break;
    case SERIAL_IER:
        val = dev->ier;
        break;
    case SERIAL_IIR:
        val = dev->iir;
        break;
    case SERIAL_LCR:
        val = dev->lcr;
        break;
    case SERIAL_MCR:
        val = dev->mcr;
        break;
    case SERIAL_LSR:
        serial_rx_poll(dev);
        val = dev->lsr;
        break;
    case SERIAL_MSR:
        val = dev->msr;
        break;
    case SERIAL_SCR:
        val = dev->scr;
        break;
    default:
        val = 0xff;
        break;
    }
    return val;
}

void serial_write(serial_dev_t *dev, uint16_t port, uint8_t val)
{
    uint8_t reg = (uint8_t)(port - SERIAL_PORT_BASE);

    /* DLAB mode */
    if (dev->lcr & 0x80) {
        if (reg == SERIAL_DLL) { dev->dll = (dev->dll & 0xff00) | val; return; }
        if (reg == SERIAL_DLH) { dev->dll = (dev->dll & 0x00ff) | ((uint16_t)val << 8); return; }
    }

    switch (reg) {
    case SERIAL_THR:
        /* Transmit: write byte to output */
        if (dev->output_fd >= 0) {
            { ssize_t _r = write(dev->output_fd, &val, 1); (void)_r; }
        }
        break;
    case SERIAL_IER:
        dev->ier = val;
        break;
    case SERIAL_FCR:
        dev->fcr = val;
        break;
    case SERIAL_LCR:
        dev->lcr = val;
        break;
    case SERIAL_MCR:
        dev->mcr = val;
        break;
    case SERIAL_SCR:
        dev->scr = val;
        break;
    default:
        break;
    }
}

/*
 * Handle a complete KVM_EXIT_IO event.
 * Returns 1 if it was a serial port, 0 if not (caller should handle).
 */
int serial_handle_io(serial_dev_t *dev, struct kvm_run *run)
{
    uint16_t port = run->io.port;

    if (port < SERIAL_PORT_BASE || port > SERIAL_PORT_END)
        return 0;

    uint8_t *data = (uint8_t *)run + run->io.data_offset;

    for (uint32_t i = 0; i < run->io.count; i++) {
        if (run->io.direction == KVM_EXIT_IO_OUT) {
            serial_write(dev, port, *data);
        } else {
            *data = serial_read(dev, port);
        }
        data += run->io.size;
    }
    return 1;
}
