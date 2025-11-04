#include "fs_utils.h"

#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FILE_CHUNK_SIZE 65536

static int join_paths(const char *a, const char *b, char *out, size_t out_len) {
    if (snprintf(out, out_len, "%s/%s", a, b) >= (int)out_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int ensure_parent_dirs(const char *path) {
    char buffer[PATH_MAX];
    size_t len = strnlen(path, sizeof(buffer));
    if (len == 0 || len >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, path, len + 1);
    for (size_t i = 1; i < len; ++i) {
        if (buffer[i] == '/') {
            buffer[i] = '\0';
            if (mkdir(buffer, 0755) < 0 && errno != EEXIST) {
                return -1;
            }
            buffer[i] = '/';
        }
    }
    return 0;
}

int sanitize_name(const char *name) {
    if (name == NULL || *name == '\0') {
        return -1;
    }
    if (strstr(name, "..") != NULL) {
        return -1;
    }
    for (const char *c = name; *c; ++c) {
        if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '_' || *c == '.')) {
            return -1;
        }
    }
    return 0;
}

int ensure_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) < 0) {
            return -1;
        }
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        return 0;
    }
    return -1;
}

int remove_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            return -1;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child[PATH_MAX];
            if (join_paths(path, entry->d_name, child, sizeof(child)) < 0) {
                closedir(dir);
                return -1;
            }
            if (remove_recursive(child) < 0) {
                closedir(dir);
                return -1;
            }
        }
        closedir(dir);
        if (rmdir(path) < 0) {
            return -1;
        }
    } else {
        if (unlink(path) < 0) {
            return -1;
        }
    }
    return 0;
}

static int send_directory_recursive(int sock, const char *base_dir, const char *relative_path) {
    char full_path[PATH_MAX];
    if (relative_path[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "%s", base_dir);
    } else if (join_paths(base_dir, relative_path, full_path, sizeof(full_path)) < 0) {
        return -1;
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child_relative[PATH_MAX];
        if (relative_path[0] == '\0') {
            snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name);
        } else if (snprintf(child_relative, sizeof(child_relative), "%s/%s", relative_path, entry->d_name) >= (int)sizeof(child_relative)) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return -1;
        }

        char child_full[PATH_MAX];
        if (join_paths(full_path, entry->d_name, child_full, sizeof(child_full)) < 0) {
            closedir(dir);
            return -1;
        }

        struct stat st;
        if (lstat(child_full, &st) < 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            size_t path_len = strnlen(child_relative, sizeof(child_relative));
            if (send_fmt(sock, "ENTRY 2 %zu 0\n", path_len) < 0) {
                closedir(dir);
                return -1;
            }
            if (send_all(sock, child_relative, path_len) < 0) {
                closedir(dir);
                return -1;
            }
            if (send_directory_recursive(sock, base_dir, child_relative) < 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            size_t path_len = strnlen(child_relative, sizeof(child_relative));
            if (send_fmt(sock, "ENTRY 1 %zu %lld\n", path_len, (long long)st.st_size) < 0) {
                closedir(dir);
                return -1;
            }
            if (send_all(sock, child_relative, path_len) < 0) {
                closedir(dir);
                return -1;
            }
            int fd = open(child_full, O_RDONLY);
            if (fd < 0) {
                closedir(dir);
                return -1;
            }
            char buffer[FILE_CHUNK_SIZE];
            ssize_t read_bytes;
            while ((read_bytes = read(fd, buffer, sizeof(buffer))) > 0) {
                if (send_all(sock, buffer, (size_t)read_bytes) < 0) {
                    close(fd);
                    closedir(dir);
                    return -1;
                }
            }
            close(fd);
            if (read_bytes < 0) {
                closedir(dir);
                return -1;
            }
        }
    }
    closedir(dir);
    return 0;
}

int send_directory_entries(int sock, const char *base_dir, const char *relative_prefix) {
    (void)relative_prefix;
    return send_directory_recursive(sock, base_dir, "");
}

int receive_world_entries(int sock, const char *target_dir) {
    char line[MCSYNC_MAX_LINE];
    while (1) {
        if (recv_line(sock, line, sizeof(line)) < 0) {
            return -1;
        }
        if (strcmp(line, "END") == 0) {
            return 0;
        }
        int type;
        unsigned long path_len;
        unsigned long long size;
        if (sscanf(line, "ENTRY %d %lu %llu", &type, &path_len, &size) != 3) {
            errno = EPROTO;
            return -1;
        }
        if (path_len == 0 || path_len >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        char *path_buffer = malloc(path_len + 1);
        if (!path_buffer) {
            return -1;
        }
        if (recv_all(sock, path_buffer, path_len) < 0) {
            free(path_buffer);
            return -1;
        }
        path_buffer[path_len] = '\0';
        if (strstr(path_buffer, "..") != NULL) {
            free(path_buffer);
            errno = EINVAL;
            return -1;
        }
        char full_path[PATH_MAX];
        if (join_paths(target_dir, path_buffer, full_path, sizeof(full_path)) < 0) {
            free(path_buffer);
            return -1;
        }
        if (type == 2) {
            if (ensure_directory(full_path, 0755) < 0) {
                free(path_buffer);
                return -1;
            }
        } else if (type == 1) {
            if (ensure_parent_dirs(full_path) < 0) {
                free(path_buffer);
                return -1;
            }
            int fd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                free(path_buffer);
                return -1;
            }
            unsigned long long remaining = size;
            char buffer[FILE_CHUNK_SIZE];
            while (remaining > 0) {
                size_t to_read = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
                if (recv_all(sock, buffer, to_read) < 0) {
                    close(fd);
                    free(path_buffer);
                    return -1;
                }
                if (write(fd, buffer, to_read) != (ssize_t)to_read) {
                    close(fd);
                    free(path_buffer);
                    return -1;
                }
                remaining -= to_read;
            }
            close(fd);
        } else {
            free(path_buffer);
            errno = EPROTO;
            return -1;
        }
        free(path_buffer);
    }
}
