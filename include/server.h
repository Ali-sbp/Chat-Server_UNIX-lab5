#ifndef SERVER_H
#define SERVER_H

#include "clients.h"
#include "config.h"
#include "logger.h"

struct server {
    int listener;
    int wakeup[2];
};

int server_open(struct server *server, const struct server_config *config,
                struct logger *log);
int server_run(struct server *server, struct clients *clients, struct logger *log);
void server_close(struct server *server);

#endif
