#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include <debug.h>
#include "devices/input.h"
#include "lib/user/syscall.h"
#include "userprog/process.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

// Validate that a user virtual address is below PHYS_BASE and mapped
static void validate_user_pointer(void *uaddr_p) {
  // Check if address is NULL or not a user address (above PHYS_BASE in kernel virtual address)

  void* uaddr = uaddr_p;
  for (int i = 0; i < sizeof(void*); i++) {
    uaddr = (void*) (((char*) uaddr_p) + i);
    if (!uaddr || !is_user_vaddr(uaddr)) {
      // printf("bad piggy1\n");
      thread_current()->info->bad_exit = true;
      thread_exit();
    }
    // Check if address is mapped in the page directory
    if (!pagedir_get_page(thread_current()->pagedir, uaddr)) {
      // printf("bad piggy2\n");
      thread_current()->info->bad_exit = true;
      thread_exit();
    }
  }
}

void validate_user_string(const char *str) {
  const char *current=str;
  while(true){
    if (!is_user_vaddr(current)||!pagedir_get_page(thread_current()->pagedir, current)) {
      thread_current()->info->bad_exit = true;
      thread_exit();
    }
    if (*current=='\0')
      break;
    current++;
  }
}

// more efficient way, check per page rather than whole buffer
void validate_user_buffer(const void *buffer, unsigned size) {
  uint8_t *start=(uint8_t*) buffer;
  uint8_t *end = start+size;

  // check first and last part of buffer and then any intermediate page boundaries
  validate_user_pointer(start);
  validate_user_pointer(end-1);

  for(uint8_t *page=(uint8_t*)pg_round_down(start)+PGSIZE; page<end; page+=PGSIZE){
    validate_user_pointer(page);
  }
}

static void syscall_handler (struct intr_frame *);
int fd_add(struct file *f);
struct file_descriptor *find_fd(int fd);
bool fd_remove(int fd);
int fd_add_file(struct file *f);
int fd_add_dir(struct dir *d);

void syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// helpers for fd syscall methods
int fd_add_file(struct file *f){
  struct thread *cur=thread_current();
  struct file_descriptor *e;
  for (int fd=2;fd<128; fd++) {
    if (cur->fd_array[fd] == NULL) {
      e=malloc(sizeof *e);
      e->is_dir=false;
      e->file=f;
      e->dir=NULL;
      cur->fd_array[fd]=e;
      return fd;
    }
  }
  return -1;
}

int fd_add_dir(struct dir *d){
  struct thread *cur = thread_current();
  struct file_descriptor *e;

  for (int fd=2;fd<128;fd++) {
    if (cur->fd_array[fd] == NULL) {
      e=malloc(sizeof *e);
      e->is_dir=true;
      e->file=NULL;
      e->dir=d;
      cur->fd_array[fd]=e;
      return fd;
    }
  }
  return -1;
}

// adding files
int fd_add(struct file *f) {
  struct thread *cur = thread_current();
  for(int fd=2; fd<128;fd++){
    if(cur->fd_array[fd]==NULL){
      cur->fd_array[fd]=f;
      return fd;
    }
  }
  return -1; // no free fds

}

// finding correlated fd to file
struct file_descriptor *find_fd(int fd){
  struct thread *cur = thread_current();
  if(fd>=128 || fd<0 ){
    return NULL;
  }
  return cur->fd_array[fd];
}

// for remove function
bool fd_remove(int fd) {
  struct thread *cur=thread_current();
  if(fd<128 && fd>=0 &&cur->fd_array[fd]!= NULL){
    struct file_descriptor *e=cur->fd_array[fd];
    if(e->is_dir){
      dir_close(e->dir);
    }else{
      file_close(e->file);
    }
    free(e);
    cur->fd_array[fd]=NULL;
    return true;
  }
  return false;
}

static void syscall_handler (struct intr_frame *f UNUSED)
{
  // check the syscall_number before dereferencing
  validate_user_pointer((void *) f->esp);
  uint32_t syscall_number = *((uint32_t*) f->esp);

  bool success;
  int fd;
  void *buffer;
  unsigned size;
  struct file *file;
  unsigned initial_size;
  unsigned position;
  struct file_descriptor *e;
  struct inode* inode;


  switch (syscall_number) {
    case SYS_HALT: 
      shutdown_power_off();
      break;

    case SYS_EXIT: 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1)); 
      int status = (int) *(((uint32_t *) f->esp) + 1);
      thread_current()->info->bad_exit = false;
      thread_current()->info->exits_gracefully = true;
      thread_current()->info->status = status;
      thread_exit();
      break;

    case SYS_EXEC: 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1));
      const char *cmd_line = (const char *) *(((uint32_t *) f->esp) + 1);
      validate_user_string(cmd_line);
      
      // No lock needed here - process_execute handles synchronization
      // It will wait on child's load_sema and return -1 if load fails
      pid_t pid = process_execute(cmd_line);
      
      f->eax = pid;  // Returns -1 if load failed, tid if success
      
      break;

    case SYS_WAIT:
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1));
      int status_w = (int) *(((uint32_t *) f->esp) + 1);
      f->eax = process_wait(status_w);
      break;

    case SYS_CREATE: 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1)); 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 2)); 

      const char *new_file = (const char *) *(((uint32_t *) f->esp) + 1);
      initial_size = (unsigned) *(((uint32_t *) f->esp) + 2);

      validate_user_string(new_file);

      // case that file name itself is null
      if(new_file==NULL){
        f->eax = false;
        break;
      }

      // file system synchronization
      success = filesys_create(new_file, initial_size);

      f->eax = success;
      break;

    case SYS_REMOVE:
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1)); 

      const char *file_name = (const char *) (*(((uint32_t*) f->esp) + 1));
      validate_user_string(file_name);

      success = filesys_remove(file_name);

      f->eax = success;
      break;

    case SYS_OPEN: 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1));

      const char *filename = (const char *) *((uint32_t *) f->esp + 1);
      
      validate_user_string(filename);

      struct file *open_file = filesys_open(filename);


      if(open_file==NULL){
        f->eax = -1;
        break;
      }

      inode=file_get_inode(open_file);
      if(inode_is_dir(inode)){
        struct dir *d=dir_open(inode_reopen(inode));
        file_close(open_file);
        if(d==NULL){
          f->eax=-1;
          break;
        }
        fd=fd_add_dir(d);
      }else{
        fd=fd_add_file(open_file);
      }      
      f->eax=fd;
      break;


    case SYS_FILESIZE:
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1)); 
      fd = (int) *(((uint32_t *) f->esp) + 1);

      e=find_fd(fd);

      if(e==NULL||e->is_dir){
        f->eax = -1;
      }else{
        f->eax= file_length(e->file);
      }

      break;

    case SYS_READ:
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1)); // fd
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 2)); // buffer
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 3)); // size

      fd = (int) *(((uint32_t *) f->esp) + 1);
      buffer = (void *) *(((uint32_t *) f->esp) + 2);
      size = (unsigned) *(((uint32_t *) f->esp) + 3);

      validate_user_buffer(buffer, size);

      if(fd==0){
        for(unsigned int i=0; i<size;i++){
          *(((uint8_t *) buffer) + i) = input_getc();
        }
        f->eax = size;
        break;
      }

      e=find_fd(fd);
      if(e==NULL||e->is_dir){
        f->eax = -1;
        break;
      }
      int bytes_read = file_read(e->file, buffer, size);
      f->eax = bytes_read;
      
      break;

    case SYS_WRITE: 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1)); 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 2)); 
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 3)); 
      
      fd = (int) *(((uint32_t *) f->esp) + 1);
      buffer = (const void *) *(((uint32_t *) f->esp) + 2);
      size = (unsigned) *(((uint32_t *) f->esp) + 3); 
      
      validate_user_buffer(buffer, size);

      // writing to console
      if(fd==1){
        putbuf(buffer, size);
        f->eax = size; 
        break;
      } 
      
      e = find_fd(fd);
      if(e==NULL||e->is_dir){
        f->eax = -1;
        break;
      }

      int bytes_written = file_write(e->file, buffer, size);

      f->eax = bytes_written;

      break;

    case SYS_SEEK:
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1)); // fd
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 2)); // position

      fd = (int) *(((uint32_t *) f->esp) + 1);
      position = (unsigned) *(((uint32_t *) f->esp) + 2);

      e = find_fd(fd);
      if(e==NULL||e->is_dir){
        break;
      }

      file_seek(e->file, position);
      
      break;

    case SYS_TELL: {
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1));

      fd = (int) *(((uint32_t *) f->esp) + 1);

      e = find_fd(fd);
      if (e==NULL||e->is_dir) {
        f->eax = 0;  
        break;
      }

      position = file_tell(e->file);

      f->eax = position; 
      break;
    }

    case SYS_CLOSE:
      validate_user_pointer((void *) (((uint32_t *) f->esp) + 1));

      fd = (int) *(((uint32_t *) f->esp) + 1);

      if (fd == 0 || fd == 1){
        break;
      }

      bool remove_success = fd_remove(fd);
      
      if(!remove_success){
        // File descriptor not found - silently ignore
      }
      
      break; 

    case SYS_CHDIR:
      validate_user_pointer((void *) (((uint32_t *) f->esp)+1));
      
      const char *dir=(const char *) *(((uint32_t *) f->esp)+1);
      validate_user_string(dir);

      bool chdir_success =filesys_chdir(dir);

      f->eax=chdir_success;
      break;
    
    // TODO Add sync
    case SYS_INUMBER:
      fd = (int) *(((uint32_t *) f->esp) + 1);
      e = thread_current()->fd_array[fd];
      if (!e->is_dir)
        inode = file_get_inode(e->file);
      else
        inode = dir_get_inode(e->dir);
      f->eax = inode_get_inumber(inode);
      break;
    case SYS_ISDIR:
      fd = (int) *(((uint32_t *) f->esp) + 1);
      e = thread_current()->fd_array[fd];
      f->eax = e->is_dir;
      break;
    case SYS_READDIR:
      fd = (int) *(((uint32_t *) f->esp) + 1);
      validate_user_pointer((void *) (((uint32_t *) f->esp)+2));
      char* name = (char*) *(((uint32_t *) f->esp) + 2);
      validate_user_buffer(name, READDIR_MAX_LEN + 1); // Not sure if we would want this to return false rather than exit

      e = thread_current()->fd_array[fd];
      if (!e->is_dir) {
        f->eax = false;
        break;
      }

      struct dir* d = e->dir;
      bool flag = 0;
      while (dir_readdir(d, name)) {
        if (!strcmp(name, ".") || !strcmp(name, ".."))
          continue;
        name[READDIR_MAX_LEN] = '\0';
        f->eax = true;
        flag = 1;
        break;
      }

      if (flag == 0) {
        f->eax = false;
      }
      break;
    case SYS_MKDIR:
      validate_user_pointer((void *) (((uint32_t *) f->esp)+1));

      const char *dir_path=(const char *) (*(((uint32_t*) f->esp)+1));
      validate_user_string(dir_path);

      success=filesys_mkdir(dir_path);

      f->eax=success;
      break;
  }
}
