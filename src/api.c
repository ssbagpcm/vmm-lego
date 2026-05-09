/*
 * api.c — Minimal REST API server using raw sockets + epoll
 *
 * Endpoints:
 *   POST   /vms
 *   GET    /vms
 *   DELETE /vms/:id
 *   POST   /vms/:id/snapshot
 *   POST   /vms/:id/restore
 *   GET    /vms/:id/stats
 */
#include "api.h"
#include "kvm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_EVENTS  64
#define BUF_SIZE    8192

/* Simple global VM registry (for API demo) */
#define MAX_API_VMS 64
typedef struct api_vm_entry {
    int   id;
    int   active;
    pid_t pid;
    char  kernel[256];
    char  rootfs[256];
    int   ram_mb;
    int   vcpus;
    int   cpu_quota;
} api_vm_entry_t;

static api_vm_entry_t g_vms[MAX_API_VMS];
static int g_next_vm_id = 1;
static pthread_mutex_t g_vms_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                         */
/* ------------------------------------------------------------------ */

static void send_response(int fd, int status, const char *body)
{
    char hdr[512];
    const char *status_str = "OK";
    if (status == 201) status_str = "Created";
    else if (status == 400) status_str = "Bad Request";
    else if (status == 404) status_str = "Not Found";
    else if (status == 500) status_str = "Internal Server Error";

    int body_len = body ? (int)strlen(body) : 0;
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_str, body_len);

    { ssize_t _wr = write(fd, hdr, hdr_len); (void)_wr; }
    if (body && body_len > 0)
        { ssize_t _wr = write(fd, body, body_len); (void)_wr; }
}

/* Minimal JSON value extraction — only handles simple string/int values */
static int json_get_string(const char *json, const char *key, char *out, int out_sz)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_sz - 1) out[i++] = *p++;
        out[i] = '\0';
        return 1;
    }
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        *out = atoi(p);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Request handling                                                     */
/* ------------------------------------------------------------------ */

static void handle_post_vms(int fd, const char *body)
{
    api_vm_entry_t vm;
    memset(&vm, 0, sizeof(vm));

    json_get_string(body, "kernel",  vm.kernel, sizeof(vm.kernel));
    json_get_string(body, "rootfs",  vm.rootfs, sizeof(vm.rootfs));
    json_get_int(body, "ram_mb",    &vm.ram_mb);
    json_get_int(body, "vcpus",     &vm.vcpus);
    json_get_int(body, "cpu_quota", &vm.cpu_quota);

    if (!vm.kernel[0] || vm.ram_mb <= 0) {
        send_response(fd, 400, "{\"error\":\"missing kernel or ram_mb\"}");
        return;
    }

    pthread_mutex_lock(&g_vms_lock);
    vm.id     = g_next_vm_id++;
    vm.active = 1;
    vm.pid    = 0;  /* vmmd would fork here in full impl */
    for (int i = 0; i < MAX_API_VMS; i++) {
        if (!g_vms[i].active) {
            g_vms[i] = vm;
            break;
        }
    }
    pthread_mutex_unlock(&g_vms_lock);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"id\":%d,\"kernel\":\"%s\",\"ram_mb\":%d,\"vcpus\":%d,\"status\":\"created\"}",
             vm.id, vm.kernel, vm.ram_mb, vm.vcpus > 0 ? vm.vcpus : 1);
    printf("[api] POST /vms → created VM %d\n", vm.id);
    send_response(fd, 201, resp);
}

static void handle_get_vms(int fd)
{
    char buf[4096];
    int  off = 0;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "[");

    pthread_mutex_lock(&g_vms_lock);
    int first = 1;
    for (int i = 0; i < MAX_API_VMS; i++) {
        if (!g_vms[i].active) continue;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "%s{\"id\":%d,\"kernel\":\"%s\",\"ram_mb\":%d}",
                        first ? "" : ",",
                        g_vms[i].id, g_vms[i].kernel, g_vms[i].ram_mb);
        first = 0;
    }
    pthread_mutex_unlock(&g_vms_lock);

    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]");
    send_response(fd, 200, buf);
}

static int find_vm(int id, api_vm_entry_t *out)
{
    for (int i = 0; i < MAX_API_VMS; i++) {
        if (g_vms[i].active && g_vms[i].id == id) {
            if (out) *out = g_vms[i];
            return i;
        }
    }
    return -1;
}

static void handle_delete_vm(int fd, int vm_id)
{
    pthread_mutex_lock(&g_vms_lock);
    int idx = find_vm(vm_id, NULL);
    if (idx < 0) {
        pthread_mutex_unlock(&g_vms_lock);
        send_response(fd, 404, "{\"error\":\"vm not found\"}");
        return;
    }
    g_vms[idx].active = 0;
    pthread_mutex_unlock(&g_vms_lock);

    printf("[api] DELETE /vms/%d\n", vm_id);
    send_response(fd, 200, "{\"status\":\"destroyed\"}");
}

static void handle_vm_snapshot(int fd, int vm_id)
{
    pthread_mutex_lock(&g_vms_lock);
    int idx = find_vm(vm_id, NULL);
    pthread_mutex_unlock(&g_vms_lock);

    if (idx < 0) {
        send_response(fd, 404, "{\"error\":\"vm not found\"}");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "/tmp/vm%d.snap", vm_id);
    printf("[api] POST /vms/%d/snapshot → %s\n", vm_id, path);
    /* In full impl: call snapshot_save() on the VM */
    char resp[512];
    snprintf(resp, sizeof(resp), "{\"snapshot\":\"%s\"}", path);
    send_response(fd, 200, resp);
}

static void handle_vm_stats(int fd, int vm_id)
{
    pthread_mutex_lock(&g_vms_lock);
    api_vm_entry_t vm;
    int idx = find_vm(vm_id, &vm);
    pthread_mutex_unlock(&g_vms_lock);

    if (idx < 0) {
        send_response(fd, 404, "{\"error\":\"vm not found\"}");
        return;
    }

    /* Read cgroup stats if available */
    char cpu_usage_str[64] = "0";
    char mem_usage_str[64] = "0";

    char cg_cpu[256], cg_mem[256];
    snprintf(cg_cpu, sizeof(cg_cpu), "/sys/fs/cgroup/vmmd/vm%d/cpu.stat", vm_id);
    snprintf(cg_mem, sizeof(cg_mem), "/sys/fs/cgroup/vmmd/vm%d/memory.current", vm_id);

    FILE *f = fopen(cg_mem, "r");
    if (f) { char *_r = fgets(mem_usage_str, sizeof(mem_usage_str), f); (void)_r; fclose(f); }
    f = fopen(cg_cpu, "r");
    if (f) { char *_r2 = fgets(cpu_usage_str, sizeof(cpu_usage_str), f); (void)_r2; fclose(f); }

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"id\":%d,\"ram_mb\":%d,\"cpu_usage\":\"%s\",\"mem_usage\":\"%s\"}",
             vm_id, vm.ram_mb, cpu_usage_str, mem_usage_str);
    send_response(fd, 200, resp);
}

/* ------------------------------------------------------------------ */
/* Request router                                                       */
/* ------------------------------------------------------------------ */

static void handle_request(int fd)
{
    char buf[BUF_SIZE];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse method and path from first line */
    char method[16], path[256];
    if (sscanf(buf, "%15s %255s", method, path) != 2) return;

    /* Find body (after \r\n\r\n) */
    const char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;

    /* Route */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/vms") == 0) {
        handle_get_vms(fd);

    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/vms") == 0) {
        handle_post_vms(fd, body ? body : "");

    } else if (strncmp(path, "/vms/", 5) == 0) {
        char *rest = path + 5;
        int vm_id = atoi(rest);
        char *slash = strchr(rest, '/');

        if (strcmp(method, "DELETE") == 0 && !slash) {
            handle_delete_vm(fd, vm_id);
        } else if (strcmp(method, "POST") == 0 && slash &&
                   strcmp(slash, "/snapshot") == 0) {
            handle_vm_snapshot(fd, vm_id);
        } else if (strcmp(method, "POST") == 0 && slash &&
                   strcmp(slash, "/restore") == 0) {
            send_response(fd, 200, "{\"status\":\"restore not yet implemented in API\"}");
        } else if (strcmp(method, "GET") == 0 && slash &&
                   strcmp(slash, "/stats") == 0) {
            handle_vm_stats(fd, vm_id);
        } else {
            send_response(fd, 404, "{\"error\":\"not found\"}");
        }
    } else {
        send_response(fd, 404, "{\"error\":\"not found\"}");
    }
}

/* ------------------------------------------------------------------ */
/* Server lifecycle                                                     */
/* ------------------------------------------------------------------ */

int api_server_init(api_server_t *srv, int port)
{
    srv->port    = port;
    srv->running = 0;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv->listen_fd < 0) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv->listen_fd);
        return -1;
    }
    if (listen(srv->listen_fd, 16) < 0) {
        perror("listen");
        close(srv->listen_fd);
        return -1;
    }

    printf("[api] listening on 127.0.0.1:%d\n", port);
    return 0;
}

void api_server_run(api_server_t *srv)
{
    srv->running = 1;

    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    struct epoll_event events[MAX_EVENTS];

    while (srv->running) {
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 500);
        for (int i = 0; i < nev; i++) {
            if (events[i].data.fd == srv->listen_fd) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);
                int cfd = accept(srv->listen_fd,
                                 (struct sockaddr *)&client_addr, &len);
                if (cfd < 0) continue;

                /* Handle synchronously (good enough for a VMM control plane) */
                handle_request(cfd);
                close(cfd);
            }
        }
    }

    close(epfd);
}

void api_server_stop(api_server_t *srv)
{
    srv->running = 0;
    close(srv->listen_fd);
}
