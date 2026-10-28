#include "chat.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Format a reply and queue its bytes for server.c to send when the socket is ready.
// Called by the welcome, message-routing, and command-handling functions below.
static int reply(struct client *client, const char *format, ...)
{
    char message[MAX_LINE_SIZE + MAX_NICK_SIZE + 128];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(message)) {
        client->failed = 1;
        client->close_reason = "message formatting failure";
        return -1;
    }
    return clients_queue(client, message, (size_t)length);
}

static char *skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t')
        ++text;
    return text;
}

/* Split one word in place; leave cursor pointing to the remaining argument text. */
static char *next_word(char **cursor)
{
    char *word = skip_spaces(*cursor);
    if (*word == '\0') {
        *cursor = word;
        return NULL;
    }
    char *end = word;
    while (*end != '\0' && *end != ' ' && *end != '\t')
        ++end;
    if (*end != '\0')
        *end++ = '\0';
    *cursor = skip_spaces(end);
    return word;
}

static int valid_nickname(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length > MAX_NICK_SIZE)
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

// Queue the initial greeting with the client's assigned guest nickname.
// Called by server.c's accept_clients() after registering a new connection.
void chat_welcome(struct client *client)
{
    reply(client, "INFO connected as %s; use /help for commands\n", client->nickname);
}

// Validate a requested nickname, check uniqueness, and confirm the name change.
// Called by chat_handle_line() for the /nick command.
static void change_nickname(struct clients *clients, struct client *client,
                            char *arguments, struct logger *log)
{
    char *name = next_word(&arguments);
    if (name == NULL || *arguments != '\0' || !valid_nickname(name)) {
        reply(client, "ERROR usage: /nick NAME (1-%d letters, digits, _ or -)\n",
              MAX_NICK_SIZE);
        return;
    }
    struct client *existing = clients_find(clients, name); // Check whether another client owns it.
    if (existing != NULL && existing != client) {
        reply(client, "ERROR nickname already in use\n");
        return;
    }
    logger_write(log, "INFO", "peer=%s nickname=%s new_nickname=%s",
                 client->peer, client->nickname, name);
    strcpy(client->nickname, name);
    reply(client, "INFO nickname is %s\n", client->nickname);
}

// Queue a private message for one named recipient and acknowledge it to the sender.
// Called by chat_handle_line() for the /msg command.
static void private_message(struct clients *clients, struct client *sender,
                            char *arguments, struct logger *log)
{
    char *name = next_word(&arguments); // Extract the recipient; arguments now holds the text.
    if (name == NULL || *arguments == '\0') {
        reply(sender, "ERROR usage: /msg NAME TEXT\n");
        return;
    }
    struct client *recipient = clients_find(clients, name);
    if (recipient == NULL) {
        reply(sender, "ERROR no such participant\n");
        return;
    }
    /* Queue to this recipient only; the sender gets a delivery-to-queue notice. */
    if (reply(recipient, "[private] %s: %s\n", sender->nickname, arguments) == -1) {
        reply(sender, "ERROR recipient cannot receive more messages\n");
        return;
    }
    reply(sender, "INFO queued for %s\n", recipient->nickname);
    logger_write(log, "INFO", "private from=%s to=%s bytes=%zu",
                 sender->nickname, recipient->nickname, strlen(arguments));
}

// Queue the same chat message for every active participant, including the sender.
// Called by chat_handle_line() for ordinary text without a leading slash.
static void broadcast(struct clients *clients, struct client *sender,
                       const char *text, struct logger *log)
{
    /* Broadcast is application-level fan-out over TCP, including the sender. */
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        struct client *recipient = &clients->items[i];
        if (recipient->fd != -1 && !recipient->closing && !recipient->failed) // Active participants only.
            reply(recipient, "[all] %s: %s\n", sender->nickname, text);
    }
    logger_write(log, "INFO", "broadcast from=%s bytes=%zu", sender->nickname, strlen(text));
}

// Validate one complete input line, then route its message or execute its command.
// Called by server.c's consume_input() whenever a newline completes a message.
void chat_handle_line(struct clients *clients, struct client *client,
                      char *line, struct logger *log)
{
    /* 1. Validate one completed line; terminal control bytes are not chat text. */
    for (const unsigned char *p = (const unsigned char *)line; *p; ++p) {
        if ((*p < 32 && *p != '\t') || *p == 127) {
            reply(client, "ERROR control characters are not allowed\n");
            return;
        }
    }
    if (*line == '\0') // Ignore empty messages.
        return;

    /* 2. Plain text is a broadcast; slash-prefixed lines are commands. */
    if (*line != '/') {
        broadcast(clients, client, line, log);
        return;
    }
    char *arguments = line;
    char *command = next_word(&arguments); // Separate the command from its remaining arguments.

    /* 3. Dispatch commands; all replies go to buffers, never directly to send(). */
    if (strcmp(command, "/nick") == 0) {
        change_nickname(clients, client, arguments, log);
    } else if (strcmp(command, "/msg") == 0) {
        private_message(clients, client, arguments, log);
    } else if (strcmp(command, "/help") == 0 && *arguments == '\0') {
        // Queue the command list for the requesting client only.
        reply(client, "INFO commands:\n"
                      "  /nick NAME     choose a unique nickname\n"
                      "  /msg NAME TEXT send a private message\n"
                      "  /who           list participants\n"
                      "  /help          show this help\n"
                      "  /quit          disconnect\n"
                      "  Plain text     broadcast to everyone, including yourself\n");
    } else if (strcmp(command, "/who") == 0 && *arguments == '\0') {
        // List active nicknames; omit unused slots and clients already leaving.
        reply(client, "INFO participants:\n");
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            struct client *listed = &clients->items[i];
            if (listed->fd != -1 && !listed->closing && !listed->failed)
                reply(client, "USER %s\n", listed->nickname);
        }
        reply(client, "INFO end of participants\n");
    } else if (strcmp(command, "/quit") == 0 && *arguments == '\0') {
        reply(client, "INFO goodbye\n");
        client->closing = 1; // Stop accepting input; server.c sends the goodbye before closing.
        client->close_reason = "quit command";
    } else {
        reply(client, "ERROR unknown command or arguments; use /help\n");
    }
}
