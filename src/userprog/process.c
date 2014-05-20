#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/kernel/hash.h"
#include "userprog/fdtable.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"

#define FILENAME_MAX_LEN 14
#define MAX_ARG_SIZE 1024

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp,
  void *aux);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  char process_name[FILENAME_MAX_LEN + 1];
  char *pch = strchr (file_name, ' ');
  int index = pch ? pch - file_name : FILENAME_MAX_LEN;
  strlcpy (process_name, file_name, index + 1);

  /* Make a copy of file_name.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    {
      return TID_ERROR;
    }
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Create a new thread to execute file_name. */
  tid = thread_create (process_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy); 
      return tid;
    }

  /* Wait for child to finish loading before returning */
  struct child_state *cs = thread_child_lookup (thread_current (), tid);
  if (!cs)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  sema_down (&cs->sema);
  if (!cs->load_success)
    {
      process_wait (tid);
      return TID_ERROR;
    }

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (thread_current ()->name, &if_.eip, &if_.esp, file_name_);
  palloc_free_page (file_name_);

  /* Inform parent of our success, or lack thereof.
     NB: Note that the parent thread _should_ exist at this point. However,
     there is a slight chance that the parent will be killed directly after we
     check for its existence and directly before we access its child list --
     the OS could, say, oom-kill it. As such, interrupts must be disabled
     during the window in which we touch the parent thread. */
  enum intr_level old_level = intr_disable ();
  struct thread *parent = thread_lookup (thread_current ()->parent_tid);
  if (parent)
    {
      struct child_state *cs =
        thread_child_lookup (parent, thread_current ()->tid);
      cs->load_success = success;
      sema_up (&cs->sema);
    }
  intr_set_level (old_level);

  /* If load failed, quit. */
  if (!success) 
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct child_state *cs = thread_child_lookup (thread_current (), child_tid);

  /* The child state will exist iff process_wait has not been previously called
     for the given child_tid. */
  if (!cs)
    {
      return -1;
    }

  sema_down (&cs->sema);
  int status = cs->exit_status;
  list_remove (&cs->elem);
  free (cs);
  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  struct hash *supp_pt;

  printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  /* Inform this process' parent, if it exists, that we are exiting.
     Interrupts must be disabled to ensure that the parent does not
     finish executing after we check for its existence but before we
     attempt to access its fields. */
  enum intr_level old_level = intr_disable ();
  struct thread *parent = thread_lookup (thread_current ()->parent_tid);
  if (parent)
    {
      struct child_state *cs = thread_child_lookup (parent,
        thread_current ()->tid);
      cs->exit_status = cur->exit_status;
      sema_up (&cs->sema);
    }
  intr_set_level (old_level);

  /* Destroy this process' list of children. */
  struct list_elem *e = list_begin (&cur->children);
  while (e != list_end (&cur->children))
    {
      struct child_state *cs = list_entry (e, struct child_state, elem);
      e = list_next (e);
      free (cs);
    }

  /* Close all open files, including the executable, and free 
   * the fd table. */
  size_t i;
  for (i = 0; i <= cur->fd_table_tail_idx; i++)
    {
      if (cur->fd_table[i] != NULL)
      {
        fd_table_close (i);
      }
    }
  free (cur->fd_table);
  
  lock_acquire (&filesys_lock);
  file_close (cur->executable);
  lock_release (&filesys_lock);

  /* TODO: Verify that we are freeing all that we should free, and that
           there are no odd race conditions or anything here. */
  supp_pt = &cur->supp_pt;
  if (supp_pt != NULL)
    {
      supp_pt_destroy (supp_pt);
    }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, const char *aux);
static bool setup_args (void **esp, const char *aux);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp, void *aux) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    {
      goto done;
    }

  /* Allocate and activate supplemental page directory. */
  if (!supp_pt_init (&t->supp_pt))
    {
      goto done;
    }

  process_activate ();

  /* Open executable file. */
  lock_acquire (&filesys_lock);
  file = filesys_open (file_name);
  lock_release (&filesys_lock);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Remember the file and deny writes to it. */
  t->executable = file;
  lock_acquire (&filesys_lock);
  file_deny_write (t->executable);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }
    lock_release (&filesys_lock);

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      lock_acquire (&filesys_lock);
      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      lock_release (&filesys_lock);

      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, aux))
    goto done;
  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not.
   * We must keep the file open in order to deny writes to it. */
  if (lock_held_by_current_thread (&filesys_lock))
    {
      lock_release (&filesys_lock);
    }
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  lock_acquire (&filesys_lock);
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    {
      lock_release (&filesys_lock);
      return false;
    }
  lock_release (&filesys_lock);

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);
  struct thread *t = thread_current ();

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      if (page_read_bytes == 0)
        {
          if (!supp_pt_page_calloc (&t->supp_pt, upage, writable))
            {
              /* TODO: For each of these error conditions: Do we need
                 to free the memory we previously allocated here? */
              return false;
          }
        }
      else if (page_zero_bytes == 0)
        {
          if (!supp_pt_page_alloc_file (&t->supp_pt,
            upage, file, ofs, PGSIZE, -1, writable))
            {
              return false;
            }
        }
      else
        {
          if (!supp_pt_page_alloc_file (&t->supp_pt, upage, file, ofs,
            page_read_bytes, -1, writable))
            {
              return false;
            }
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      ofs += PGSIZE;
      upage += PGSIZE;
    }

  return true;
}

static bool
setup_args (void **esp, const char *aux)
{
  char *curr;
  char *token, *save_ptr;
  int argc = 0;

  /* Limit total length of aux to one page */
  if (strlen(aux) > MAX_ARG_SIZE)
    return false;

  for (token = strtok_r ((char *)aux, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      *esp = (char*)*esp - strlen(token) - 1;
      memcpy(*esp, token, strlen(token) + 1);
      argc++;
    }

  /* Remember the location of our last argument. */
  curr = *esp;

  /* Round esp down to a word size. */
  *esp = (char *)*esp - ((uintptr_t)*esp) % sizeof(uint32_t);

  /* Insert NULL sentinel value. */
  *esp = (char **)*esp - 1;
  *((char **)*esp) = NULL;

  /* Make room for pointers to arguments. */
  *esp = (char **)*esp - argc;
  char **argv = *esp;

  int i;
  for (i = 0; i < argc; i++)
    {
      argv[argc - i - 1] = curr;
      curr += strlen(curr) + 1;
    }

  *esp = (char ***)*esp - 1;
  *((char ***)*esp) = argv;

  *esp = (int *)*esp - 1;
  *(int *)*esp = argc;

  *esp = (void **)*esp - 1;
  *(void **)*esp = NULL;

  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory, and by installing the supplied arguments
   onto the stack. The aux parameter should be a string of arguments
   separated by whitespace. */
static bool
setup_stack (void **esp, const char *aux) 
{
  bool success = false;
  struct thread *t = thread_current ();
  void *upage = (void *) (PHYS_BASE - PGSIZE);

  if (supp_pt_page_calloc (&t->supp_pt, upage, true) &&
    page_handle_fault (&t->supp_pt, upage))
    {
      *esp = PHYS_BASE;
      success = setup_args(esp, aux);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
