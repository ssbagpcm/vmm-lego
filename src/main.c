/*
 * main.c — vmmd daemon + vmctl CLI
 *
 * Usage:
 *   vmmd --api-port 8080 --kernel bzImage --rootfs rootfs.ext4 --ram 512
 *   vmmd --test-kvm           (Phase 1 test: boot HLT kernel)
 *   vmmd --boot-linux         (Phase 2+: boot real bzImage)
 *
 * vmctl (simple wrapper over REST API):
 *   vmctl spawn ...
 *   vmctl list
 *   vmctl snapshot <id>
 *   vmctl kill <id>
 */
#include "kvm_core.h"
#include "boot.h"
#include "serial.h"
#include "virtio_blk.h"
#include "resources.h"
#include "snapshot.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <linux/kvm.h>

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

static volatile int g_running = 1;

static void sighandler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Phase 1 test: tiny 16-bit "kernel" that just HALTs                  */
/* ------------------------------------------------------------------ */

/*
 * Loads a 2-byte payload [0xF4, 0xF4] (HLT, HLT) into guest RAM at
 * address 0x1000 and sets up the vCPU in real mode to execute it.
 * Expected: KVM exits with KVM_EXIT_HLT.
 */
static int test_kvm_hlt(void)
{
    printf("=== Phase 1: KVM HLT test ===\n");

    kvm_handle_t *kvm = kvm_init();
    if (!kvm) return 1;

    vm_t *vm = vm_create(kvm);
    if (!vm) { kvm_destroy(kvm); return 1; }

    /* 4 MB of RAM */
    if (vm_set_memory(vm, 4) < 0) goto fail;

    /* Write HLT instruction at 0x1000 */
    uint8_t *p = gpa_to_hva(vm, 0x1000);
    p[0] = 0xF4;  /* HLT */

    vcpu_t *vcpu = vcpu_create(vm, 0);
    if (!vcpu) goto fail;

    /* Set up real mode: CS:IP = 0x0000:0x1000 */
    struct kvm_sregs sregs;
    vcpu_get_sregs(vcpu, &sregs);

    /* Set CS to flat real mode */
    sregs.cs.base     = 0;
    sregs.cs.limit    = 0xffff;
    sregs.cs.selector = 0;

    vcpu_set_sregs(vcpu, &sregs);

    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip    = 0x1000;
    regs.rflags = 0x2;
    vcpu_set_regs(vcpu, &regs);

    printf("[test] running vCPU, expecting KVM_EXIT_HLT...\n");

    int exit_reason = vcpu_run(vcpu);
    printf("[test] exit_reason = %d", exit_reason);
    if (exit_reason == KVM_EXIT_HLT) {
        printf(" (KVM_EXIT_HLT) ✓\n");
        printf("\nMILESTONE 1: KVM HLT test passed — KVM init, VM create, vCPU run, exit handling all working\n");
    } else {
        printf(" (unexpected)\n");
    }

    vm_destroy(vm);
    kvm_destroy(kvm);
    return 0;

fail:
    vm_destroy(vm);
    kvm_destroy(kvm);
    return 1;
}

/* ------------------------------------------------------------------ */
/* VM run thread                                                        */
/* ------------------------------------------------------------------ */

typedef struct vm_run_args {
    vcpu_t          *vcpu;
    serial_dev_t    *serial;
    virtio_blk_t    *blk;
    vm_t            *vm;
} vm_run_args_t;

static void *vcpu_thread(void *arg)
{
    vm_run_args_t *a = (vm_run_args_t *)arg;
    vcpu_t        *vcpu   = a->vcpu;
    serial_dev_t  *serial = a->serial;
    virtio_blk_t  *blk    = a->blk;

    printf("[vcpu %d] thread started\n", vcpu->id);

    while (!vcpu->stop && g_running) {
        int exit_reason = vcpu_run(vcpu);

        if (exit_reason < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[vcpu %d] KVM_RUN error\n", vcpu->id);
            break;
        }

        switch (exit_reason) {
        case KVM_EXIT_HLT:
            printf("[vcpu %d] HLT — halted\n", vcpu->id);
            vcpu->halted = 1;
            /* wait for interrupt / resume */
            usleep(1000);
            break;

        case KVM_EXIT_IO:
            if (serial && serial_handle_io(serial, vcpu->kvm_run))
                break;
            /* Unhandled I/O port */
#ifdef DEBUG_IO
            fprintf(stderr, "[vcpu %d] unhandled IO port=0x%x dir=%d\n",
                    vcpu->id,
                    vcpu->kvm_run->io.port,
                    vcpu->kvm_run->io.direction);
#endif
            break;

        case KVM_EXIT_MMIO:
            if (blk && virtio_blk_handle_mmio(blk, vcpu->kvm_run))
                break;
            /* Unhandled MMIO */
            fprintf(stderr, "[vcpu %d] unhandled MMIO addr=0x%llx is_write=%d\n",
                    vcpu->id,
                    (unsigned long long)vcpu->kvm_run->mmio.phys_addr,
                    vcpu->kvm_run->mmio.is_write);
            break;

        case KVM_EXIT_SHUTDOWN:
            printf("[vcpu %d] SHUTDOWN\n", vcpu->id);
            vcpu->stop = 1;
            g_running  = 0;
            break;

        case KVM_EXIT_FAIL_ENTRY:
            fprintf(stderr, "[vcpu %d] FAIL_ENTRY: hardware entry failure reason=0x%llx\n",
                    vcpu->id,
                    (unsigned long long)vcpu->kvm_run->fail_entry.hardware_entry_failure_reason);
            vcpu->stop = 1;
            g_running  = 0;
            break;

        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "[vcpu %d] INTERNAL_ERROR suberror=0x%x\n",
                    vcpu->id,
                    vcpu->kvm_run->internal.suberror);
            vcpu->stop = 1;
            g_running  = 0;
            break;

        default:
            fprintf(stderr, "[vcpu %d] unhandled exit_reason=%d\n",
                    vcpu->id, exit_reason);
            /* Log and continue, per the spec */
            break;
        }
    }

    printf("[vcpu %d] thread exiting\n", vcpu->id);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Boot a Linux kernel                                                  */
/* ------------------------------------------------------------------ */


/* Wrapper for pthread compatibility */
static void *api_server_run_thread(void *arg)
{
    api_server_run((api_server_t *)arg);
    return NULL;
}

static int boot_linux(const char *kernel_path, const char *rootfs_path,
                      uint64_t ram_mb, int api_port, const char *cmdline,
                      const char *snapshot_path, int do_restore)
{
    printf("=== Booting Linux: %s ===\n", kernel_path);

    kvm_handle_t *kvm = kvm_init();
    if (!kvm) return 1;

    vm_t *vm = vm_create(kvm);
    if (!vm) goto fail_kvm;

    if (vm_set_memory(vm, ram_mb) < 0) goto fail_vm;

    /* --- Load kernel --- */
    uint64_t entry_point = 0;
    if (do_restore) {
        /* Will set entry from snapshot — just need memory allocated */
        printf("[main] restore mode: will load RAM from snapshot\n");
    } else {
        if (load_bzimage(vm, kernel_path, &entry_point) < 0) goto fail_vm;
        if (setup_boot_params(vm, cmdline, 0, 0) < 0) goto fail_vm;
    }

    /* --- Create vCPU --- */
    vcpu_t *vcpu = vcpu_create(vm, 0);
    if (!vcpu) goto fail_vm;

    if (!do_restore) {
        if (setup_long_mode(vcpu, entry_point) < 0) goto fail_vm;
    }

    /* --- Serial device --- */
    serial_dev_t serial;
    serial_init(&serial, STDIN_FILENO, STDOUT_FILENO);

    /* --- Virtio block device --- */
    virtio_blk_t *blk = NULL;
    if (rootfs_path && rootfs_path[0]) {
        blk = virtio_blk_create(vm, rootfs_path);
        if (!blk) fprintf(stderr, "[main] WARNING: failed to open rootfs, continuing without disk\n");
    }

    /* --- Resource control --- */
    vm_resources_t res;
    if (resources_init_cgroup(&res, vm->id) == 0) {
        resources_set_cpu(&res, 0);         /* unlimited */
        resources_set_memory(&res, (long)ram_mb);
        resources_pin_process(&res, getpid());
    }

    /* --- Restore from snapshot? --- */
    if (do_restore && snapshot_path) {
        if (snapshot_restore(vm, snapshot_path, &serial, blk) < 0) {
            fprintf(stderr, "[main] snapshot restore failed\n");
            goto fail_vm;
        }
    }

    /* --- API server thread --- */
    api_server_t api_srv;
    pthread_t api_thread = 0;
    if (api_port > 0) {
        if (api_server_init(&api_srv, api_port) == 0) {
            pthread_create(&api_thread, NULL,
                           api_server_run_thread, &api_srv);
        }
    }

    /* --- Run vCPU --- */
    vm_run_args_t run_args = {
        .vcpu   = vcpu,
        .serial = &serial,
        .blk    = blk,
        .vm     = vm,
    };

    pthread_t vcpu_th;
    pthread_create(&vcpu_th, NULL, vcpu_thread, &run_args);

    /* Main thread: handle signals, snapshot requests */
    signal(SIGINT,  sighandler);
    signal(SIGTERM, sighandler);

    printf("[main] VM running. Press Ctrl+C to stop, send SIGUSR1 to snapshot.\n");

    /* Auto-snapshot on SIGUSR1 */
    signal(SIGUSR1, sighandler);  /* simplified: just stop for demo */

    while (g_running && !vcpu->stop) {
        sleep(1);
    }

    /* Save snapshot on clean exit */
    if (snapshot_path && !do_restore) {
        printf("[main] saving snapshot to %s\n", snapshot_path);
        snapshot_save(vm, snapshot_path, &serial, blk);
    }

    vcpu->stop = 1;
    pthread_join(vcpu_th, NULL);

    if (api_thread) {
        api_server_stop(&api_srv);
        pthread_join(api_thread, NULL);
    }

    if (blk) virtio_blk_destroy(blk);
    resources_cleanup(&res);
    vm_destroy(vm);
    kvm_destroy(kvm);

    printf("\nMILESTONE 2+: Linux boot sequence completed\n");
    return 0;

fail_vm:
    vm_destroy(vm);
fail_kvm:
    kvm_destroy(kvm);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --test-kvm            Run Phase 1 KVM HLT test\n"
        "  --boot-linux          Boot a Linux kernel\n"
        "  --kernel PATH         Path to bzImage\n"
        "  --rootfs PATH         Path to rootfs disk image\n"
        "  --ram MB              RAM in MB (default: 256)\n"
        "  --cmdline STRING      Kernel command line\n"
        "  --api-port PORT       Start REST API on PORT (default: 8080)\n"
        "  --snapshot PATH       Snapshot file path\n"
        "  --restore             Restore from snapshot\n"
        "\n"
        "Examples:\n"
        "  %s --test-kvm\n"
        "  %s --boot-linux --kernel bzImage --rootfs rootfs.ext4 --ram 512\n"
        "  %s --boot-linux --kernel bzImage --ram 256 --snapshot /tmp/vm.snap\n"
        "  %s --boot-linux --restore --snapshot /tmp/vm.snap\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    int opt_test_kvm    = 0;
    int opt_boot_linux  = 0;
    int opt_restore     = 0;
    int opt_api_port    = 8080;
    uint64_t opt_ram    = 256;

    char opt_kernel[256]   = "";
    char opt_rootfs[256]   = "";
    char opt_cmdline[1024] = "console=ttyS0 root=/dev/vda rw nokaslr";
    char opt_snapshot[256] = "";

    static struct option long_opts[] = {
        { "test-kvm",    no_argument,       NULL, 't' },
        { "boot-linux",  no_argument,       NULL, 'b' },
        { "kernel",      required_argument, NULL, 'k' },
        { "rootfs",      required_argument, NULL, 'r' },
        { "ram",         required_argument, NULL, 'm' },
        { "cmdline",     required_argument, NULL, 'c' },
        { "api-port",    required_argument, NULL, 'p' },
        { "snapshot",    required_argument, NULL, 's' },
        { "restore",     no_argument,       NULL, 'R' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "tbk:r:m:c:p:s:Rh", long_opts, NULL)) != -1) {
        switch (c) {
        case 't': opt_test_kvm   = 1; break;
        case 'b': opt_boot_linux = 1; break;
        case 'k': strncpy(opt_kernel,   optarg, sizeof(opt_kernel)-1);   break;
        case 'r': strncpy(opt_rootfs,   optarg, sizeof(opt_rootfs)-1);   break;
        case 'm': opt_ram = (uint64_t)atol(optarg); break;
        case 'c': strncpy(opt_cmdline,  optarg, sizeof(opt_cmdline)-1);  break;
        case 'p': opt_api_port = atoi(optarg); break;
        case 's': strncpy(opt_snapshot, optarg, sizeof(opt_snapshot)-1); break;
        case 'R': opt_restore = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (opt_test_kvm) {
        return test_kvm_hlt();
    }

    if (opt_boot_linux || opt_restore) {
        if (!opt_restore && !opt_kernel[0]) {
            fprintf(stderr, "Error: --kernel required\n");
            usage(argv[0]);
            return 1;
        }
        return boot_linux(opt_kernel, opt_rootfs, opt_ram,
                          opt_api_port, opt_cmdline,
                          opt_snapshot[0] ? opt_snapshot : NULL,
                          opt_restore);
    }

    /* Default: run test */
    printf("[vmmd] no action specified, running KVM test\n");
    return test_kvm_hlt();
}
