#include "clients.h"
#include "config.h"
#include "daemon.h"
#include "logger.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    /* 1. Read defaults and command-line overrides. */
    struct server_config config;
    int result = config_parse(argc, argv, &config);
    if (result != 0)
        return result == 1 ? EXIT_SUCCESS : EXIT_FAILURE;

    /* 2. Prepare logging, client storage, and the non-blocking listening socket. */
    struct logger log;
    if (logger_open(&log, config.log_path) == -1) {
        perror("log file");
        return EXIT_FAILURE;
    }
    struct clients clients;
    if (clients_init(&clients) == -1) {
        logger_write(&log, "ERROR", "client storage: %s", strerror(errno));
        fprintf(stderr, "Cannot allocate client storage; see %s\n", config.log_path);
        logger_close(&log);
        return EXIT_FAILURE;
    }
    struct server server;
    if (server_open(&server, &config, &log) == -1) {
        fprintf(stderr, "Cannot start server; see %s\n", config.log_path);
        clients_close(&clients);
        logger_close(&log);
        return EXIT_FAILURE;
    }

    /* 3. Detach unless foreground debugging was requested. */
    if (!config.foreground && daemon_detach() == -1) {
        logger_write(&log, "ERROR", "daemon setup: %s", strerror(errno));
        result = -1;
    } else {
        logger_write(&log, "INFO", "started bind=%s port=%d mode=%s max_clients=%d",
                     config.bind_address, config.port,
                     config.foreground ? "foreground" : "daemon", MAX_CLIENTS);
        if (config.foreground) {
            printf("Chat server: %s:%d (Ctrl+C to stop)\n",
                   config.bind_address, config.port);
            fflush(stdout);
        }

        /* 4. Run the select() loop; chat commands queue messages for ready sockets. */
        result = server_run(&server, &clients, &log);
        logger_write(&log, "INFO", "stopped; closing %zu remaining clients", clients.count);
    }

    /* 5. Release the server's sockets, client buffers, and log descriptor. */
    server_close(&server);
    clients_close(&clients);
    logger_close(&log);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
