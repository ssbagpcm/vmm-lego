#ifndef SERIAL_H
#define SERIAL_H

#include "kvm_core.h"
#include <stdint.h>

/* 16550 UART base port (COM1) */
#define SERIAL_PORT_BASE    0x3f8
#define SERIAL_PORT_END     0x3ff

/* Register offsets */
#define SERIAL_RBR  0  /* Receive Buffer Register (read) */
#define SERIAL_THR  0  /* Transmit Holding Register (write) */
#define SERIAL_IER  1  /* Interrupt Enable Register */
#define SERIAL_IIR  2  /* Interrupt Identification Register (read) */
#define SERIAL_FCR  2  /* FIFO Control Register (write) */
#define SERIAL_LCR  3  /* Line Control Register */
#define SERIAL_MCR  4  /* Modem Control Register */
#define SERIAL_LSR  5  /* Line Status Register */
#define SERIAL_MSR  6  /* Modem Status Register */
#define SERIAL_SCR  7  /* Scratch Register */
#define SERIAL_DLL  0  /* Divisor Latch Low (when LCR[7]=1) */
#define SERIAL_DLH  1  /* Divisor Latch High (when LCR[7]=1) */

/* LSR bits */
#define LSR_DR      0x01  /* Data Ready */
#define LSR_THRE    0x20  /* THR Empty */
#define LSR_TEMT    0x40  /* Transmitter Empty */

typedef struct serial_dev {
    uint8_t rbr;   /* receive buffer */
    uint8_t ier;   /* interrupt enable */
    uint8_t iir;   /* interrupt id */
    uint8_t fcr;   /* FIFO control */
    uint8_t lcr;   /* line control */
    uint8_t mcr;   /* modem control */
    uint8_t lsr;   /* line status */
    uint8_t msr;   /* modem status */
    uint8_t scr;   /* scratch */
    uint16_t dll;  /* divisor latch */

    int     input_fd;   /* stdin fd */
    int     output_fd;  /* stdout fd */
} serial_dev_t;

void serial_init(serial_dev_t *dev, int input_fd, int output_fd);
uint8_t serial_read(serial_dev_t *dev, uint16_t port);
void    serial_write(serial_dev_t *dev, uint16_t port, uint8_t val);

/* Handle a KVM_EXIT_IO event for serial ports */
int serial_handle_io(serial_dev_t *dev, struct kvm_run *run);

#endif /* SERIAL_H */
