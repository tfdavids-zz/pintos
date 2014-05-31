#ifndef USERPROG_FDTABLE_H
#define USERPROG_FDTABLE_H

#include "filesys/file.h"

struct fd_entry;

int fd_table_open_file (const char *file);
int fd_table_open_dir (const char *dir);

struct file *fd_table_get_file (int fd);
struct dir *fd_table_get_dir (int fd);

bool fd_table_close (int fd);
void fd_table_dispose (void);

bool fd_table_is_valid_fd (int fd);

#endif /* userprog/fdtable.h */
