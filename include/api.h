#ifndef API_H
#define API_H

#include <stdint.h>

/* REST API server */
typedef struct api_server {
    int     port;
    int     listen_fd;
    int     running;
} api_server_t;

int  api_server_init(api_server_t *srv, int port);
void api_server_run(api_server_t *srv);   /* blocking */
void api_server_stop(api_server_t *srv);

#endif /* API_H */
