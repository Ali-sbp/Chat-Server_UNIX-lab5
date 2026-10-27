#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --port NUMBER     TCP port (default: 3425)\n"
           "  --bind ADDRESS    Local IPv4 address (default: 0.0.0.0)\n"
           "  --log FILE        Log file (default: ./server.log)\n"
           "  --foreground      Stay attached to the terminal\n"
           "  --help            Show this help\n", program);
}

static int parse_port(const char *text, int *port)
{
    char *end;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > 65535)
        return -1;
    *port = (int)value;
    return 0;
}

int config_parse(int argc, char **argv, struct server_config *config)
{
    const char *log_path = "./server.log";
    const struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"bind", required_argument, NULL, 'b'},
        {"log", required_argument, NULL, 'l'},
        {"foreground", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    *config = (struct server_config){.port = 3425};
    strcpy(config->bind_address, "0.0.0.0");

    /* 1. Apply command-line overrides to the defaults. */
    int option;
    while ((option = getopt_long(argc, argv, "p:b:l:fh", options, NULL)) != -1) {
        switch (option) {
        case 'p':
            if (parse_port(optarg, &config->port) == -1) {
                fprintf(stderr, "Port must be between 1 and 65535\n");
                return -1;
            }
            break;
        case 'b': {
            struct in_addr address;
            if (strlen(optarg) >= sizeof(config->bind_address) ||
                inet_pton(AF_INET, optarg, &address) != 1) {
                fprintf(stderr, "Bind address must be a numeric IPv4 address\n");
                return -1;
            }
            strcpy(config->bind_address, optarg);
            break;
        }
        case 'l': log_path = optarg; break;
        case 'f': config->foreground = 1; break;
        case 'h': usage(argv[0]); return 1;
        default: return -1;
        }
    }
    if (optind != argc || *log_path == '\0') {
        fprintf(stderr, "Unexpected argument or empty log path; use --help\n");
        return -1;
    }

    /* 2. Resolve the log path before daemonization changes the working directory. */
    int length;
    if (log_path[0] == '/') {
        length = snprintf(config->log_path, sizeof(config->log_path), "%s", log_path);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd");
            return -1;
        }
        length = snprintf(config->log_path, sizeof(config->log_path),
                          "%s/%s", cwd, log_path);
    }
    if (length < 0 || (size_t)length >= sizeof(config->log_path)) {
        fprintf(stderr, "Log path is too long\n");
        return -1;
    }
    return 0;
}
