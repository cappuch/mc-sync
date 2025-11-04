#ifndef MCSYNC_FS_UTILS_H
#define MCSYNC_FS_UTILS_H

#include <sys/types.h>
#include <sys/stat.h>

int sanitize_name(const char *name);
int ensure_directory(const char *path, mode_t mode);
int remove_recursive(const char *path);
int send_directory_entries(int sock, const char *base_dir, const char *relative_prefix);
int receive_world_entries(int sock, const char *target_dir);

#endif /* MCSYNC_FS_UTILS_H */
