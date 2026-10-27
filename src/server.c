#include "server.h"
#include "chat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

// The signal handler only changes atomic flags and writes to a non-blocking pipe.
static volatile sig_atomic_t stopping = 0;
static volatile sig_atomic_t signal_write_fd = -1;

static void request_stop(int signal_number)
{
    (void)signal_number;
    int saved_errno = errno;
    stopping = 1;
    if (signal_write_fd != -1) {
        char byte = 1;
        ssize_t ignored = write((int)signal_write_fd, &byte, 1);
        (void)ignored;
    }
    errno = saved_errno;
}

static int make_nonblocking(int fd)
{
    /* Preserve existing flags; CLOEXEC prevents leaking sockets into exec(). */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return -1;
    flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
        return -1;
    return 0;
}

static int configure_signals(int wakeup_fd)
{
    signal_write_fd = wakeup_fd;
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = request_stop;
    if (sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1)
        return -1;

    // A disconnected peer should make send() fail, not terminate the server.
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

void server_close(struct server *server)
{
    signal_write_fd = -1;
    if (server->listener != -1)
        close(server->listener);
    for (int i = 0; i < 2; ++i) {
        if (server->wakeup[i] != -1)
            close(server->wakeup[i]);
        server->wakeup[i] = -1;
    }
    server->listener = -1;
}

int server_open(struct server *server, const struct server_config *config,
                struct logger *log)
{
    *server = (struct server){.listener = -1, .wakeup = {-1, -1}};
    stopping = 0;

    /* 1. Create a non-blocking TCP listener and allow rebinding after restart.
       socket: IPv4, byte stream, default protocol (TCP). */
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener == -1 || make_nonblocking(server->listener) == -1)
        goto failure;
    int reuse = 1;
    if (setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1)
        goto failure;

    /* 2. Bind the configured local address and begin accepting connections. */
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)config->port);
    if (inet_pton(AF_INET, config->bind_address, &address.sin_addr) != 1) {
        errno = EINVAL;
        goto failure;
    }
    if (bind(server->listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(server->listener, 16) == -1)
        goto failure;

    /* 3. Wake select() reliably when a shutdown signal arrives.
       The pipe also covers a signal arriving immediately before select(). */
    if (pipe(server->wakeup) == -1 || make_nonblocking(server->wakeup[0]) == -1 ||
        make_nonblocking(server->wakeup[1]) == -1)
        goto failure;
    if (server->listener >= FD_SETSIZE || server->wakeup[0] >= FD_SETSIZE) {
        errno = EMFILE;
        goto failure;
    }
    if (configure_signals(server->wakeup[1]) == -1)
        goto failure;
    return 0;

failure:
    {
        int error = errno;
        logger_write(log, "ERROR", "server setup: %s", strerror(error));
        server_close(server);
        errno = error;
        return -1;
    }
}

static int accept_clients(struct server *server, struct clients *clients,
                           struct logger *log)
{
    /* Accept a bounded batch so a connection flood cannot starve existing clients. */
    for (int attempt = 0; attempt < 16 && !stopping; ++attempt) {
        struct sockaddr_in address;
        socklen_t size = sizeof(address);
        int fd = accept(server->listener, (struct sockaddr *)&address, &size);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            if (errno == ECONNABORTED)
                continue;
            /* Stop on persistent resource errors instead of spinning and logging. */
            logger_write(log, "ERROR", "accept: %s", strerror(errno));
            return -1;
        }
        if (fd >= FD_SETSIZE || make_nonblocking(fd) == -1) {
            if (fd >= FD_SETSIZE)
                logger_write(log, "WARN", "rejected descriptor beyond FD_SETSIZE");
            else
                logger_write(log, "ERROR", "client flags: %s", strerror(errno));
            close(fd);
            continue;
        }

        char ip[INET_ADDRSTRLEN] = "unknown", peer[64];
        inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
        snprintf(peer, sizeof(peer), "%s:%u", ip, (unsigned int)ntohs(address.sin_port));
        struct client *client = clients_add(clients, fd, peer);
        if (client == NULL) {
            logger_write(log, "WARN", "peer=%s rejected: %s", peer, strerror(errno));
            close(fd);  // Registration failed; ownership never passed to clients.c.
            continue;
        }
        chat_welcome(client);
        logger_write(log, "INFO", "connected peer=%s nickname=%s clients=%zu",
                     client->peer, client->nickname, clients->count);
    }
    return 0;
}

static void reject_line(struct client *client, const char *message, const char *reason)
{
    clients_queue(client, message, strlen(message));
    client->closing = 1;
    client->close_reason = reason;
}

static void consume_input(struct clients *clients, struct client *client,
                           const char *data, size_t length, struct logger *log)
{
    /* TCP delivers bytes, not messages: keep partial lines and split combined ones. */
    for (size_t i = 0; i < length && !client->closing && !client->failed; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\0') {
            reject_line(client, "ERROR null bytes are not allowed\n", "null byte in input");
            return;
        }
        if (c == '\n') {
            // Accept both LF and CRLF, without exposing the terminator to chat.c.
            size_t used = client->input_used;
            if (used > 0 && client->input[used - 1] == '\r')
                --used;
            if (used > MAX_LINE_SIZE) {
                reject_line(client, "ERROR line too long\n", "input line limit");
                return;
            }
            client->input[used] = '\0';
            chat_handle_line(clients, client, client->input, log);
            client->input_used = 0;
        } else {
            // Reserve one extra byte for the optional CR after a maximum-length line.
            if (client->input_used == MAX_LINE_SIZE + 1) {
                reject_line(client, "ERROR line too long\n", "input line limit");
                return;
            }
            client->input[client->input_used++] = (char)c;
        }
    }
}

static void read_client(struct clients *clients, struct client *client,
                         struct logger *log)
{
    // Read one chunk per ready client, then give other clients their turn.
    char buffer[4096];
    ssize_t n = recv(client->fd, buffer, sizeof(buffer), 0);
    if (n > 0) {
        consume_input(clients, client, buffer, (size_t)n, log);
    } else if (n == 0) {
        // A peer may stop sending but still read replies; flush before closing.
        client->closing = 1;
        client->close_reason = "peer finished sending";
        if (client->input_used != 0)
            logger_write(log, "WARN", "nickname=%s discarded incomplete final line",
                         client->nickname);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        logger_write(log, "ERROR", "recv nickname=%s: %s", client->nickname, strerror(errno));
        client->failed = 1;
        client->close_reason = "receive error";
    }
}

static void write_client(struct client *client, struct logger *log)
{
    /* Send only queued bytes; a partial send leaves the remainder for later.
       Socket, first unsent byte, unsent byte count, no extra flags. */
    ssize_t n = send(client->fd, client->output + client->output_offset,
                     client->output_used, 0);
    if (n > 0) {
        client->output_offset += (size_t)n;
        client->output_used -= (size_t)n;
        if (client->output_used == 0)
            client->output_offset = 0;
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        logger_write(log, "ERROR", "send nickname=%s: %s", client->nickname,
                     n == 0 ? "no progress" : strerror(errno));
        client->failed = 1;
        client->close_reason = "send error";
    }
}

static void remove_finished(struct clients *clients, struct logger *log)
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        struct client *client = &clients->items[i];
        if (client->fd != -1 &&
            (client->failed || (client->closing && client->output_used == 0))) {
            logger_write(log, client->failed ? "WARN" : "INFO",
                         "disconnected peer=%s nickname=%s reason=%s",
                         client->peer, client->nickname, client->close_reason);
            clients_remove(clients, client);
        }
    }
}

int server_run(struct server *server, struct clients *clients, struct logger *log)
{
    while (!stopping) {
        /* 1. Build fresh sets: listener and active readers, plus pending writers. */
        remove_finished(clients, log);
        fd_set readers, writers;
        FD_ZERO(&readers);
        FD_ZERO(&writers);
        FD_SET(server->listener, &readers);
        FD_SET(server->wakeup[0], &readers);
        int maximum = server->listener > server->wakeup[0]
                      ? server->listener : server->wakeup[0];
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            struct client *client = &clients->items[i];
            if (client->fd == -1)
                continue;
            if (!client->closing)
                FD_SET(client->fd, &readers);
            if (client->output_used != 0)
                FD_SET(client->fd, &writers);
            if (client->fd > maximum)
                maximum = client->fd;
        }

        /* 2. Sleep until activity or a shutdown signal; no busy polling or idle exit.
           Highest descriptor + 1, read set, write set, no exception set or timeout. */
        int ready = select(maximum + 1, &readers, &writers, NULL, NULL);
        if (ready == -1) {
            if (errno == EINTR)
                continue;
            logger_write(log, "ERROR", "select: %s", strerror(errno));
            return -1;
        }
        if (stopping || FD_ISSET(server->wakeup[0], &readers))
            break;

        /* 3. Accept newcomers, then advance each ready connection independently. */
        if (FD_ISSET(server->listener, &readers) &&
            accept_clients(server, clients, log) == -1)
            return -1;
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            struct client *client = &clients->items[i];
            if (client->fd == -1 || client->failed)
                continue;
            if (!client->closing && FD_ISSET(client->fd, &readers))
                read_client(clients, client, log);
            if (!client->failed && FD_ISSET(client->fd, &writers))
                write_client(client, log);
        }

        /* 4. Remove failed clients or those whose final replies have been sent. */
        remove_finished(clients, log);
    }
    return 0;
}
