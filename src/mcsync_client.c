#include "platform.h"
#include "common.h"
#include "fs_utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    char host[256];
    int port;
} mc_config_t;

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s init <host> <port>\n"
            "  %s list\n"
            "  %s push <world_dir> [world_name]\n"
            "  %s pull <world_name> <destination_dir>\n",
            prog, prog, prog, prog);
}

static int load_config(const char *config_path, mc_config_t *config) {
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        return -1;
    }
    char line[256];
    bool has_host = false;
    bool has_port = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "host=", 5) == 0) {
            strncpy(config->host, line + 5, sizeof(config->host) - 1);
            config->host[sizeof(config->host) - 1] = '\0';
            size_t len = strlen(config->host);
            if (len > 0 && config->host[len - 1] == '\n') {
                config->host[len - 1] = '\0';
            }
            has_host = true;
        } else if (strncmp(line, "port=", 5) == 0) {
            config->port = atoi(line + 5);
            has_port = config->port > 0 && config->port <= 65535;
        }
    }
    fclose(fp);
    if (!has_host || !has_port) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int write_config(const char *config_path, const mc_config_t *config) {
    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "host=%s\nport=%d\n", config->host, config->port);
    fclose(fp);
    return 0;

}

static int find_config_path(char *buffer, size_t buffer_len) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        return -1;
    }
    if (snprintf(buffer, buffer_len, "%s/.mcsync/config", cwd) >= (int)buffer_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    struct stat st;
    if (stat(buffer, &st) == 0 && S_ISREG(st.st_mode)) {
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static int connect_to_remote(const mc_config_t *config) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", config->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result;
    int rc = getaddrinfo(config->host, port_str, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }
    int sock = -1;
    for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            freeaddrinfo(result);
            return sock;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(result);
    return -1;
}

static const char *basename_safely(const char *path) {
    const char *end = path + strlen(path);
    while (end > path && end[-1] == '/') {
        --end;
    }
    if (end == path) {
        return path;
    }
    const char *slash = end - 1;
    while (slash >= path && *slash != '/') {
        --slash;
    }
    return slash + 1;
}

static int cmd_init(const char *host, const char *port_str) {
    mc_config_t config;
    memset(&config, 0, sizeof(config));
    strncpy(config.host, host, sizeof(config.host) - 1);
    config.port = atoi(port_str);
    if (config.port <= 0 || config.port > 65535) {
        fprintf(stderr, "invalid port: %s\n", port_str);
        return -1;
    }
    if (ensure_directory(".mcsync", 0755) < 0) {
        perror("mkdir .mcsync");
        return -1;
    }
    char config_path[PATH_MAX];
    if (snprintf(config_path, sizeof(config_path), ".mcsync/config") >= (int)sizeof(config_path)) {
        fprintf(stderr, "config path too long\n");
        return -1;
    }
    if (write_config(config_path, &config) < 0) {
        perror("write config");
        return -1;
    }
    printf("Initialized mcsync remote at %s:%d\n", config.host, config.port);
    return 0;
}

static int wait_for_done_or_error(int sock) {
    char line[MCSYNC_MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) < 0) {
        return -1;
    }
    if (strncmp(line, "ERR ", 4) == 0) {
        fprintf(stderr, "Server error: %s\n", line + 4);
        return -1;
    }
    if (strcmp(line, "DONE") == 0) {
        return 0;
    }
    fprintf(stderr, "Unexpected response: %s\n", line);
    return -1;
}

static int cmd_list(const mc_config_t *config) {
    int sock = connect_to_remote(config);
    if (sock < 0) {
        perror("connect");
        return -1;
    }
    if (send_fmt(sock, "LIST\n") < 0) {
        perror("send");
        close(sock);
        return -1;
    }
    char line[MCSYNC_MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) < 0) {
        perror("recv");
        close(sock);
        return -1;
    }
    if (strncmp(line, "ERR ", 4) == 0) {
        fprintf(stderr, "Server error: %s\n", line + 4);
        close(sock);
        return -1;
    }
    unsigned long count;
    if (sscanf(line, "COUNT %lu", &count) != 1) {
        fprintf(stderr, "Unexpected response: %s\n", line);
        close(sock);
        return -1;
    }
    for (unsigned long i = 0; i < count; ++i) {
        if (recv_line(sock, line, sizeof(line)) < 0) {
            perror("recv");
            close(sock);
            return -1;
        }
        unsigned long name_len;
        if (sscanf(line, "WORLD %lu", &name_len) != 1) {
            fprintf(stderr, "Unexpected response: %s\n", line);
            close(sock);
            return -1;
        }
        char *name = malloc(name_len + 1);
        if (!name) {
            fprintf(stderr, "Out of memory\n");
            close(sock);
            return -1;
        }
        if (recv_all(sock, name, name_len) < 0) {
            perror("recv");
            free(name);
            close(sock);
            return -1;
        }
        name[name_len] = '\0';
        printf("%s\n", name);
        free(name);
    }
    if (wait_for_done_or_error(sock) < 0) {
        close(sock);
        return -1;
    }
    close(sock);
    return 0;
}

static int cmd_push(const mc_config_t *config, const char *world_dir, const char *world_name_override) {
    struct stat st;
    if (stat(world_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "World directory not found: %s\n", world_dir);
        return -1;
    }
    const char *base_name = world_name_override ? world_name_override : basename_safely(world_dir);
    if (sanitize_name(base_name) < 0) {
        fprintf(stderr, "Invalid world name: %s\n", base_name);
        return -1;
    }
    size_t name_len = strlen(base_name);
    int sock = connect_to_remote(config);
    if (sock < 0) {
        perror("connect");
        return -1;
    }
    if (send_fmt(sock, "PUSH %zu\n", name_len) < 0) {
        perror("send");
        close(sock);
        return -1;
    }
    if (send_all(sock, base_name, name_len) < 0) {
        perror("send");
        close(sock);
        return -1;
    }
    char line[MCSYNC_MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) < 0) {
        perror("recv");
        close(sock);
        return -1;
    }
    if (strncmp(line, "ERR ", 4) == 0) {
        fprintf(stderr, "Server error: %s\n", line + 4);
        close(sock);
        return -1;
    }
    if (strcmp(line, "OK") != 0) {
        fprintf(stderr, "Unexpected response: %s\n", line);
        close(sock);
        return -1;
    }
    if (send_directory_entries(sock, world_dir, "") < 0) {
        perror("send world data");
        close(sock);
        return -1;
    }
    if (send_fmt(sock, "END\n") < 0) {
        perror("send");
        close(sock);
        return -1;
    }
    if (wait_for_done_or_error(sock) < 0) {
        close(sock);
        return -1;
    }
    printf("Pushed world '%s'\n", base_name);
    close(sock);
    return 0;
}

static int cmd_pull(const mc_config_t *config, const char *world_name, const char *destination_dir) {
    if (sanitize_name(world_name) < 0) {
        fprintf(stderr, "Invalid world name: %s\n", world_name);
        return -1;
    }
    if (ensure_directory(destination_dir, 0755) < 0) {
        perror("destination");
        return -1;
    }
    size_t name_len = strlen(world_name);
    int sock = connect_to_remote(config);
    if (sock < 0) {
        perror("connect");
        return -1;
    }
    if (send_fmt(sock, "PULL %zu\n", name_len) < 0) {
        perror("send");
        close(sock);
        return -1;
    }
    if (send_all(sock, world_name, name_len) < 0) {
        perror("send");
        close(sock);
        return -1;
    }
    char line[MCSYNC_MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) < 0) {
        perror("recv");
        close(sock);
        return -1;
    }
    if (strncmp(line, "ERR ", 4) == 0) {
        fprintf(stderr, "Server error: %s\n", line + 4);
        close(sock);
        return -1;
    }
    if (strcmp(line, "FOUND") != 0) {
        fprintf(stderr, "Unexpected response: %s\n", line);
        close(sock);
        return -1;
    }
    if (receive_world_entries(sock, destination_dir) < 0) {
        fprintf(stderr, "Failed to receive world data\n");
        close(sock);
        return -1;
    }
    if (wait_for_done_or_error(sock) < 0) {
        close(sock);
        return -1;
    }
    printf("Pulled world '%s' into %s\n", world_name, destination_dir);
    close(sock);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char *command = argv[1];
    if (strcmp(command, "init") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (cmd_init(argv[2], argv[3]) < 0) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    char config_path[PATH_MAX];
    mc_config_t config;
    if (find_config_path(config_path, sizeof(config_path)) < 0) {
        fprintf(stderr, "Unable to locate .mcsync/config in current directory\n");
        return EXIT_FAILURE;
    }
    if (load_config(config_path, &config) < 0) {
        fprintf(stderr, "Failed to parse config: %s\n", config_path);
        return EXIT_FAILURE;
    }
    if (strcmp(command, "list") == 0) {
        if (argc != 2) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (cmd_list(&config) < 0) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    if (strcmp(command, "push") == 0) {
        if (argc != 3 && argc != 4) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (cmd_push(&config, argv[2], argc == 4 ? argv[3] : NULL) < 0) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    if (strcmp(command, "pull") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (cmd_pull(&config, argv[2], argv[3]) < 0) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    print_usage(argv[0]);
    return EXIT_FAILURE;
}
