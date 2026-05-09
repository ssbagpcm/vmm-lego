/*
 * vmm_base.c - Fork-based Process Isolation VMM Proof of Concept
 *
 * Concept: Boot a "base" process (simulating a kernel), then fork() it
 * to create isolated "VM" instances.  Each VM starts with a *copy* of
 * the base RAM image (like cloning a qcow2 backing file) and can modify
 * it independently.  A second MAP_SHARED slab lets the parent peek at
 * the child's live memory state.  Each child is isolated via Linux
 * namespaces (UTS, IPC, MNT).
 *
 * Build: gcc -o vmm vmm_base.c -lpthread
 * Run:   sudo ./vmm   (namespace ops need CAP_SYS_ADMIN)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------- */
#define MAX_VMS          16
#define SLAB_SIZE        4096          /* bytes per RAM slab              */
#define HOSTNAME_PREFIX  "vm"
#define REPL_PROMPT      "vmm> "

/* -----------------------------------------------------------------------
 * GuestRAM - the simulated "physical memory" of one VM
 * --------------------------------------------------------------------- */
typedef struct {
    int    counter;          /* simple integer the guest bumps            */
    char   message[256];     /* string the guest can overwrite            */
    pid_t  owner_pid;        /* last writer's PID                         */
} GuestRAM;

/* -----------------------------------------------------------------------
 * VMDesc - parent's bookkeeping for one running VM
 * --------------------------------------------------------------------- */
typedef struct {
    int      id;
    pid_t    pid;
    char     hostname[32];
    time_t   start_time;
    int      alive;
    /*
     * shared_ram: MAP_SHARED anonymous region shared between parent and
     * this specific child.  The child writes here; parent reads it for
     * 'status' / cowdemo.  This is how the parent "peeks inside the VM".
     */
    GuestRAM *shared_ram;
} VMDesc;

/* -----------------------------------------------------------------------
 * Globals
 * --------------------------------------------------------------------- */
static GuestRAM *base_ram  = NULL;
static VMDesc    vm_table[MAX_VMS];
static int       vm_count  = 0;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

/* -----------------------------------------------------------------------
 * STEP 1 – initialise the base "kernel" RAM image
 * --------------------------------------------------------------------- */
static GuestRAM *alloc_ram(int shared)
{
    int flags = PROT_READ | PROT_WRITE;
    int mflags = (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS;
    GuestRAM *r = mmap(NULL, SLAB_SIZE, flags, mflags, -1, 0);
    if (r == MAP_FAILED) { perror("mmap"); exit(1); }
    return r;
}

static GuestRAM *init_base_ram(void)
{
    GuestRAM *ram = alloc_ram(0);   /* private – only base touches this  */
    ram->counter   = 42;
    ram->owner_pid = getpid();
    snprintf(ram->message, sizeof(ram->message),
             "Base kernel image  (pid=%d)", (int)getpid());
    printf("[base] Guest RAM at %p\n", (void *)ram);
    printf("[base] Initial state: counter=%d  msg=\"%s\"\n",
           ram->counter, ram->message);
    return ram;
}

/* -----------------------------------------------------------------------
 * Helper: pretty-print a GuestRAM snapshot
 * --------------------------------------------------------------------- */
static void print_ram(const char *label, const GuestRAM *r)
{
    printf("  [%-10s]  counter=%-6d  owner_pid=%-7d  msg=\"%s\"\n",
           label, r->counter, (int)r->owner_pid, r->message);
}

/* -----------------------------------------------------------------------
 * STEP 2 – guest workload (runs inside each child/VM)
 * --------------------------------------------------------------------- */
static void guest_workload(int vm_id, GuestRAM *shared_ram,
                           const GuestRAM *start_snapshot)
{
    char hn[32];
    snprintf(hn, sizeof(hn), "%s%d", HOSTNAME_PREFIX, vm_id);

    /* --- Namespace isolation ----------------------------------------- */
    int ns_flags = CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS;
    if (unshare(ns_flags) < 0) {
        fprintf(stderr, "[vm%d] unshare: %s (continuing without)\n",
                vm_id, strerror(errno));
    }
    if (sethostname(hn, strlen(hn)) < 0) {
        fprintf(stderr, "[vm%d] sethostname: %s\n", vm_id, strerror(errno));
    }

    pid_t my_pid = getpid();
    printf("[vm%d] Booted  host=%-10s  pid=%d\n", vm_id, hn, (int)my_pid);
    printf("[vm%d] Inherited RAM: counter=%d  msg=\"%s\"\n",
           vm_id, start_snapshot->counter, start_snapshot->message);

    /* --- STEP 3: modify shared_ram so parent can observe CoW divergence */
    shared_ram->counter   = start_snapshot->counter + vm_id * 10;
    shared_ram->owner_pid = my_pid;
    snprintf(shared_ram->message, sizeof(shared_ram->message),
             "Written by vm%d  pid=%d", vm_id, (int)my_pid);

    printf("[vm%d] RAM modified: counter=%d  msg=\"%s\"\n",
           vm_id, shared_ram->counter, shared_ram->message);
    fflush(stdout);

    /* --- Keep running so parent can inspect us with 'list'/'status' --- */
    int tick = 0;
    while (1) {
        sleep(1);
        tick++;
        shared_ram->counter++;
        if (tick % 5 == 0) {
            printf("[vm%d] tick=%-3d  counter=%d\n",
                   vm_id, tick, shared_ram->counter);
            fflush(stdout);
        }
    }
}

/* -----------------------------------------------------------------------
 * STEP 2 – spawn_vm()
 * --------------------------------------------------------------------- */
static int spawn_vm(void)
{
    pthread_mutex_lock(&vm_lock);

    if (vm_count >= MAX_VMS) {
        printf("[base] VM table full (max %d)\n", MAX_VMS);
        pthread_mutex_unlock(&vm_lock);
        return -1;
    }

    int vm_id = vm_count;

    /*
     * Allocate a MAP_SHARED region that will be visible to both parent
     * (for status queries) and the specific child (for writes).
     * We initialise it from base_ram so the child starts with the same
     * state as the base image.
     */
    GuestRAM *shared_ram = alloc_ram(1);      /* MAP_SHARED            */
    memcpy(shared_ram, base_ram, sizeof(GuestRAM));

    /*
     * Take a snapshot of the initial RAM state to pass into the child
     * so it can print "what it started with" even after it overwrites
     * shared_ram.
     */
    GuestRAM snapshot;
    memcpy(&snapshot, base_ram, sizeof(GuestRAM));

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        munmap(shared_ram, SLAB_SIZE);
        pthread_mutex_unlock(&vm_lock);
        return -1;
    }

    if (pid == 0) {
        /* ---- CHILD ---- */
        pthread_mutex_unlock(&vm_lock);
        guest_workload(vm_id, shared_ram, &snapshot);
        _exit(0);
    }

    /* ---- PARENT ---- */
    VMDesc *v     = &vm_table[vm_count++];
    v->id         = vm_id;
    v->pid        = pid;
    v->start_time = time(NULL);
    v->alive      = 1;
    v->shared_ram = shared_ram;
    snprintf(v->hostname, sizeof(v->hostname),
             "%s%d", HOSTNAME_PREFIX, vm_id);

    printf("[base] Spawned VM %d  pid=%d  hostname=%s\n",
           vm_id, (int)pid, v->hostname);

    pthread_mutex_unlock(&vm_lock);
    return vm_id;
}

/* -----------------------------------------------------------------------
 * Reap finished VMs (non-blocking)
 * --------------------------------------------------------------------- */
static void reap_vms(void)
{
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < vm_count; i++) {
            if (vm_table[i].pid == p) {
                vm_table[i].alive = 0;
                printf("[base] VM %d (pid=%d) exited\n",
                       vm_table[i].id, (int)p);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * STEP 4 – CLI commands
 * --------------------------------------------------------------------- */
static void cmd_list(void)
{
    reap_vms();
    printf("%-4s %-8s %-12s %-10s %s\n",
           "ID","PID","HOSTNAME","UPTIME","STATUS");
    printf("%-4s %-8s %-12s %-10s %s\n",
           "----","--------","------------","----------","------");
    time_t now = time(NULL);
    for (int i = 0; i < vm_count; i++) {
        VMDesc *v = &vm_table[i];
        int up = (int)(now - v->start_time);
        printf("%-4d %-8d %-12s %-10ds %s\n",
               v->id, (int)v->pid, v->hostname,
               up, v->alive ? "running" : "dead");
    }
    if (vm_count == 0) printf("  (no VMs)\n");
}

static void cmd_kill(int id)
{
    if (id < 0 || id >= vm_count) {
        printf("[base] Unknown VM id %d\n", id); return;
    }
    VMDesc *v = &vm_table[id];
    if (!v->alive) { printf("[base] VM %d already dead\n", id); return; }
    if (kill(v->pid, SIGTERM) == 0) {
        printf("[base] Sent SIGTERM to VM %d (pid=%d)\n", id, (int)v->pid);
        /* give it a moment to die, then reap */
        usleep(300000);
        reap_vms();
    } else {
        perror("kill");
    }
}

static void cmd_status(int id)
{
    if (id < 0 || id >= vm_count) {
        printf("[base] Unknown VM id %d\n", id); return;
    }
    VMDesc *v = &vm_table[id];
    printf("VM %-2d  pid=%-8d  hostname=%-12s  alive=%s\n",
           v->id, (int)v->pid, v->hostname, v->alive ? "yes" : "no");
    printf("  Live RAM (via shared mapping):\n");
    print_ram(v->hostname, v->shared_ram);
}

/* -----------------------------------------------------------------------
 * STEP 3 – CoW demo: spawn 3 VMs, show memory diverges from base
 * --------------------------------------------------------------------- */
static void cmd_cow_demo(void)
{
    printf("\n=== CoW Memory Demo ===\n");
    printf("Base RAM BEFORE spawning any VMs:\n");
    print_ram("base", base_ram);
    printf("\nSpawning 3 VMs...\n\n");

    int ids[3];
    for (int i = 0; i < 3; i++) {
        ids[i] = spawn_vm();
    }

    /* Give children time to write their modifications */
    fflush(stdout);
    sleep(2);

    printf("\n--- Memory states after VMs modified their copies ---\n\n");

    printf("Base RAM (should be UNCHANGED from above):\n");
    print_ram("base", base_ram);
    printf("\n");

    for (int i = 0; i < 3; i++) {
        if (ids[i] >= 0) {
            VMDesc *v = &vm_table[ids[i]];
            printf("VM %d live RAM (independent copy):\n", ids[i]);
            print_ram(v->hostname, v->shared_ram);
        }
    }

    printf("\n>>> base.counter=%d  vm0.counter=%d  vm1.counter=%d  vm2.counter=%d\n",
           base_ram->counter,
           (ids[0] >= 0 ? vm_table[ids[0]].shared_ram->counter : -1),
           (ids[1] >= 0 ? vm_table[ids[1]].shared_ram->counter : -1),
           (ids[2] >= 0 ? vm_table[ids[2]].shared_ram->counter : -1));
    printf(">>> All different = CoW isolation confirmed ✓\n");
    printf("=== End CoW Demo ===\n\n");
}

static void print_help(void)
{
    printf("\nCommands:\n");
    printf("  spawn          – create a new VM clone\n");
    printf("  list           – show running VMs (pid, hostname, uptime)\n");
    printf("  kill <id>      – terminate VM by id\n");
    printf("  status <id>    – show live memory state of VM\n");
    printf("  cowdemo        – spawn 3 VMs and verify CoW isolation\n");
    printf("  basestate      – print base RAM\n");
    printf("  help           – this message\n");
    printf("  quit / exit    – shutdown all VMs and exit\n\n");
}

/* -----------------------------------------------------------------------
 * STEP 4 – REPL
 * --------------------------------------------------------------------- */
static void run_repl(void)
{
    char line[256];
    print_help();

    while (1) {
        reap_vms();
        printf(REPL_PROMPT);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "spawn") == 0) {
            spawn_vm();

        } else if (strcmp(line, "list") == 0) {
            cmd_list();

        } else if (strncmp(line, "kill ", 5) == 0) {
            cmd_kill(atoi(line + 5));

        } else if (strncmp(line, "status ", 7) == 0) {
            cmd_status(atoi(line + 7));

        } else if (strcmp(line, "cowdemo") == 0) {
            cmd_cow_demo();

        } else if (strcmp(line, "basestate") == 0) {
            printf("Base RAM:\n");
            print_ram("base", base_ram);

        } else if (strcmp(line, "help") == 0) {
            print_help();

        } else if (strcmp(line, "quit") == 0 ||
                   strcmp(line, "exit") == 0) {
            printf("[base] Shutting down – killing all VMs\n");
            for (int i = 0; i < vm_count; i++)
                if (vm_table[i].alive)
                    kill(vm_table[i].pid, SIGKILL);
            break;

        } else if (line[0] != '\0') {
            printf("Unknown command '%s'  (type 'help')\n", line);
        }
    }
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    printf("=== Fork-based VMM PoC ===\n");
    printf("Host PID: %d\n", (int)getpid());

    base_ram = init_base_ram();
    run_repl();

    /* Cleanup */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    munmap(base_ram, SLAB_SIZE);
    printf("[base] Goodbye.\n");
    return 0;
}
