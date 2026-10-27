#ifndef CLIENTS_H
#define CLIENTS_H

#include <stddef.h>
#include "config.h"

struct client {
    int fd;                         /* -1 marks a free slot. */
    char nickname[MAX_NICK_SIZE + 1];
    char peer[64];
    char input[MAX_LINE_SIZE + 2];  /* One line, optional CR, and final zero. */
    size_t input_used;
    char output[MAX_PENDING_SIZE];
    size_t output_offset;
    size_t output_used;             /* Number of unsent bytes, not a string length. */
    int closing;                   /* Stop reading; finish queued output first. */
    int failed;                    /* Close immediately, discarding queued output. */
    const char *close_reason;
};

struct clients {
    struct client *items;
    size_t count;
    unsigned long next_id;
};

int clients_init(struct clients *clients);
void clients_close(struct clients *clients);
/* add takes ownership of fd on success only; remove closes an owned socket. */
struct client *clients_add(struct clients *clients, int fd, const char *peer);
void clients_remove(struct clients *clients, struct client *client);
struct client *clients_find(struct clients *clients, const char *nickname);
/* Queue bytes without sending; overflow marks this client for removal. */
int clients_queue(struct client *client, const char *data, size_t length);

#endif
