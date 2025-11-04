#include "platform.h"
#include "common.h"
#include "fs_utils.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

static int join_paths(const char *a, const char *b, char *out, size_t out_len) {
    if (snprintf(out, out_len, "%s/%s", a, b) >= (int)out_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int send_error(int sock, const char *message) {
    return send_fmt(sock, "ERR %s\n", message);
}

static int handle_push(int client_fd, const char *storage_dir, const char *line) {
    unsigned long name_len;
    if (sscanf(line, "PUSH %lu", &name_len) != 1) {
        return send_error(client_fd, "InvalidCommand");
    }
    if (name_len == 0 || name_len >= PATH_MAX) {
        return send_error(client_fd, "InvalidName");
    }
    char *world_name = malloc(name_len + 1);
    if (!world_name) {
        return send_error(client_fd, "OutOfMemory");
    }
    if (recv_all(client_fd, world_name, name_len) < 0) {
        free(world_name);
        return -1;
    }
    world_name[name_len] = '\0';
    if (sanitize_name(world_name) < 0) {
        send_error(client_fd, "InvalidName");
        free(world_name);
        return -1;
    }
    if (send_fmt(client_fd, "OK\n") < 0) {
        free(world_name);
        return -1;
    }
    char tmp_template[PATH_MAX];
    if (snprintf(tmp_template, sizeof(tmp_template), "%s/.%s.tmpXXXXXX", storage_dir, world_name) >= (int)sizeof(tmp_template)) {
        send_error(client_fd, "ServerError");
        free(world_name);
        return -1;
    }
    if (ensure_directory(storage_dir, 0755) < 0) {
        send_error(client_fd, "ServerError");
        free(world_name);
        return -1;
    }
    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) {
        send_error(client_fd, "ServerError");
        free(world_name);
        return -1;
    }
    if (receive_world_entries(client_fd, tmp_dir) < 0) {
        send_error(client_fd, "ReceiveFailed");
        remove_recursive(tmp_dir);
        free(world_name);
        return -1;
    }
    char world_path[PATH_MAX];
    if (join_paths(storage_dir, world_name, world_path, sizeof(world_path)) < 0) {
        send_error(client_fd, "ServerError");
        remove_recursive(tmp_dir);
        free(world_name);
        return -1;
    }
    if (remove_recursive(world_path) < 0) {
        send_error(client_fd, "ServerError");
        remove_recursive(tmp_dir);
        free(world_name);
        return -1;
    }
    if (rename(tmp_dir, world_path) < 0) {
        send_error(client_fd, "ServerError");
        remove_recursive(tmp_dir);
        free(world_name);
        return -1;
    }
    if (send_fmt(client_fd, "DONE\n") < 0) {
        free(world_name);
        return -1;
    }
    free(world_name);
    return 0;
}

static int handle_pull(int client_fd, const char *storage_dir, const char *line) {
    unsigned long name_len;
    if (sscanf(line, "PULL %lu", &name_len) != 1) {
        return send_error(client_fd, "InvalidCommand");
    }
    if (name_len == 0 || name_len >= PATH_MAX) {
        return send_error(client_fd, "InvalidName");
    }
    char *world_name = malloc(name_len + 1);
    if (!world_name) {
        return send_error(client_fd, "OutOfMemory");
    }
    if (recv_all(client_fd, world_name, name_len) < 0) {
        free(world_name);
        return -1;
    }
    world_name[name_len] = '\0';
    if (sanitize_name(world_name) < 0) {
        send_error(client_fd, "InvalidName");
        free(world_name);
        return -1;
    }
    char world_path[PATH_MAX];
    if (join_paths(storage_dir, world_name, world_path, sizeof(world_path)) < 0) {
        send_error(client_fd, "ServerError");
        free(world_name);
        return -1;
    }
    struct stat st;
    if (stat(world_path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        send_error(client_fd, "NotFound");
        free(world_name);
        return -1;
    }
    if (send_fmt(client_fd, "FOUND\n") < 0) {
        free(world_name);
        return -1;
    }
    if (send_directory_entries(client_fd, world_path, "") < 0) {
        free(world_name);
        return -1;
    }
    if (send_fmt(client_fd, "END\nDONE\n") < 0) {
        free(world_name);
        return -1;
    }
    free(world_name);
    return 0;
}

static int handle_list(int client_fd, const char *storage_dir) {
    if (ensure_directory(storage_dir, 0755) < 0) {
        return send_error(client_fd, "ServerError");
    }
    DIR *dir = opendir(storage_dir);
    if (!dir) {
        return send_error(client_fd, "ServerError");
    }
    struct dirent *entry;
    size_t count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char full_path[PATH_MAX];
        if (join_paths(storage_dir, entry->d_name, full_path, sizeof(full_path)) < 0) {
            closedir(dir);
            return send_error(client_fd, "ServerError");
        }
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ++count;
        }
    }
    rewinddir(dir);
    if (send_fmt(client_fd, "COUNT %zu\n", count) < 0) {
        closedir(dir);
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char full_path[PATH_MAX];
        if (join_paths(storage_dir, entry->d_name, full_path, sizeof(full_path)) < 0) {
            closedir(dir);
            return send_error(client_fd, "ServerError");
        }
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t name_len = strnlen(entry->d_name, PATH_MAX);
            if (send_fmt(client_fd, "WORLD %zu\n", name_len) < 0) {
                closedir(dir);
                return -1;
            }
            if (send_all(client_fd, entry->d_name, name_len) < 0) {
                closedir(dir);
                return -1;
            }
        }
    }
    closedir(dir);
    if (send_fmt(client_fd, "DONE\n") < 0) {
        return -1;
    }
    return 0;
}

static void handle_client(int client_fd, const char *storage_dir) {
    char line[MCSYNC_MAX_LINE];
    if (recv_line(client_fd, line, sizeof(line)) < 0) {
        return;
    }
    if (strncmp(line, "PUSH ", 5) == 0) {
        handle_push(client_fd, storage_dir, line);
    } else if (strncmp(line, "PULL ", 5) == 0) {
        handle_pull(client_fd, storage_dir, line);
    } else if (strcmp(line, "LIST") == 0) {
        handle_list(client_fd, storage_dir);
    } else {
        send_error(client_fd, "UnknownCommand");
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -d <storage_dir> [-p port]\n", prog);
}

int main(int argc, char **argv) {
    const char *storage_dir = NULL;
    int port = 25570;
    int opt;
    while ((opt = getopt(argc, argv, "d:p:")) != -1) {
        switch (opt) {
        case 'd':
            storage_dir = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (!storage_dir) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (ensure_directory(storage_dir, 0755) < 0) {
        perror("storage directory");
        return EXIT_FAILURE;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }
    printf("mcsync server listening on port %d, storage dir %s\n", port, storage_dir);
    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(client_fd, storage_dir);
        close(client_fd);
    }
    close(listen_fd);
    printf("mcsync server shutting down\n");
    return EXIT_SUCCESS;
}
