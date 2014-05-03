#ifndef USERPROG_FDTABLE_H
#define USERPROG_FDTABE_H

#include "filesys/file.h"

int fd_table_open (const char *file);
struct file *fd_table_get_file (int fd);
bool fd_table_close (int fd);
bool fd_table_is_valid_fd (int fd);

#endif /* userprog/fdtable.h */
