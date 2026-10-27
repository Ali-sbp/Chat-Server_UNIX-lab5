#include "clients.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int clients_init(struct clients *clients)
{
    /* Keep the large per-client buffers on the heap, not main()'s stack. */
    *clients = (struct clients){.next_id = 1};
    clients->items = calloc(MAX_CLIENTS, sizeof(*clients->items));
    if (clients->items == NULL)
        return -1;
    for (size_t i = 0; i < MAX_CLIENTS; ++i)
        clients->items[i].fd = -1;
    return 0;
}

struct client *clients_find(struct clients *clients, const char *nickname)
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        struct client *client = &clients->items[i];
        if (client->fd != -1 && !client->closing && !client->failed &&
            strcmp(client->nickname, nickname) == 0)
            return client;
    }
    return NULL;
}

struct client *clients_add(struct clients *clients, int fd, const char *peer)
{
    if (clients->count == MAX_CLIENTS) {
        errno = ENOSPC;
        return NULL;
    }
    struct client *client = NULL;
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (clients->items[i].fd == -1) {
            client = &clients->items[i];
            break;
        }
    }
    if (client == NULL) {
        errno = ENOSPC;
        return NULL;
    }

    /* Give every connection a usable name, skipping names already taken by /nick. */
    char nickname[MAX_NICK_SIZE + 1];
    do {
        if (clients->next_id == 0) {
            errno = EOVERFLOW;
            return NULL;
        }
        int length = snprintf(nickname, sizeof(nickname), "guest%lu", clients->next_id++);
        if (length < 0 || (size_t)length >= sizeof(nickname)) {
            errno = EOVERFLOW;
            return NULL;
        }
    } while (clients_find(clients, nickname) != NULL);

    memset(client, 0, sizeof(*client));
    client->fd = fd;
    strcpy(client->nickname, nickname);
    snprintf(client->peer, sizeof(client->peer), "%s", peer);
    ++clients->count;
    return client;
}

int clients_queue(struct client *client, const char *data, size_t length)
{
    if (client->failed)
        return -1;

    /* A slow reader must not make the server allocate unlimited outgoing data. */
    if (length > MAX_PENDING_SIZE - client->output_used) {
        client->failed = 1;
        client->close_reason = "outgoing buffer limit (slow client)";
        return -1;
    }
    /* Reuse space left by earlier partial sends before appending new bytes. */
    if (client->output_offset + client->output_used + length > MAX_PENDING_SIZE) {
        memmove(client->output, client->output + client->output_offset,
                client->output_used);
        client->output_offset = 0;
    }
    memcpy(client->output + client->output_offset + client->output_used, data, length);
    client->output_used += length;
    return 0;
}

void clients_remove(struct clients *clients, struct client *client)
{
    if (client->fd == -1)
        return;
    /* This module owns registered sockets; removing a slot closes its connection. */
    close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    --clients->count;
}

void clients_close(struct clients *clients)
{
    if (clients->items == NULL)
        return;
    for (size_t i = 0; i < MAX_CLIENTS; ++i)
        clients_remove(clients, &clients->items[i]);
    free(clients->items);
    clients->items = NULL;
}
