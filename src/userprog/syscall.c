#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#define MAX_ARGS 3
static uint8_t syscall_arg_num =
  [0, 1, 1, 1, 2, 1, 1, 1, 3, 3, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1];


static void syscall_handler (struct intr_frame *);
static void sys_halt (void) NO_RETURN;
static void sys_exit (int status) NO_RETURN;
static pid_t sys_exec (const char *file);
static int sys_wait (pid_t);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned length);
static int sys_write (int fd, const void *buffer, unsigned length);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  uint32_t args[MAX_ARGS];
  void *intr_esp = f->esp;
  uint32_t syscall_num = *((uint32_t *)intr_esp);
  uint8_t arg_num = syscall_arg_num[syscall_num];
  
  for (int i = 0; i < arg_num; i++)
    {
      ((uint32_t *)intr_esp)++;
      args[i] = *((uint32_t *)intr_esp);
    }

  switch (syscall_num)
    {
      case SYS_HALT:
      case SYS_EXIT:
      case SYS_EXEC:
      case SYS_WAIT:
      case SYS_CREATE:
      case SYS_REMOVE:
      case SYS_OPEN:
      case SYS_FILESIZE:
      case SYS_READ:
      case SYS_WRITE:
      case SYS_SEEK:
      case SYS_TELL:
      case SYS_CLOSE:
      case SYS_MMAP:
      case SYS_MUNMAP:
      case SYS_CHDIR:
      case SYS_MKDIR:
      case SYS_READDIR:
      case SYS_READDIR:
      case SYS_ISDIR:
      case SYS_INUMBER:
    }

  /* TODO: Invoke an appropriate helper function for that syscall. */
}

/* TODO: Validate user memory. */
static void sys_halt (void) NO_RETURN;
static void sys_exit (int status) NO_RETURN;
static pid_t sys_exec (const char *file);
static int sys_wait (pid_t);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned length);
static int sys_write (int fd, const void *buffer, unsigned length);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);
