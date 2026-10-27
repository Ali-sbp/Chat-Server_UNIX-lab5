#ifndef CHAT_H
#define CHAT_H

#include "clients.h"
#include "logger.h"

void chat_welcome(struct client *client);
/* Interpret one complete line in place and queue replies/messages. */
void chat_handle_line(struct clients *clients, struct client *client,
                      char *line, struct logger *log);

#endif
