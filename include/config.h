#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Fixed lab limits; runtime settings are collected in one structure. */
#define MAX_CLIENTS 64
#define MAX_NICK_SIZE 24
#define MAX_LINE_SIZE 1024
#define MAX_PENDING_SIZE 65536

struct server_config {
    int port;
    int foreground;
    char bind_address[16];
    char log_path[PATH_MAX];
};

/* Return 0 to run, 1 after --help, or -1 for invalid settings. */
int config_parse(int argc, char **argv, struct server_config *config);

#endif
