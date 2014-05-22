#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

typedef int pid_t;
#define PID_ERROR ((pid_t) -1)

typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

void syscall_init (void);

#endif /* userprog/syscall.h */
