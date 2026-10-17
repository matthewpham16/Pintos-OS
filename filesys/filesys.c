#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include <string.h>
#include "threads/thread.h"
#include "threads/malloc.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

static struct dir *get_directory_filename(char *filename, const char *path, struct dir *cwd);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
  struct dir* root_dir = dir_open_root();
  if (!root_dir)
    PANIC("Could not get root directory in file system initialization");
  
  thread_current()->cwd = root_dir;
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done (void) { free_map_close (); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
   // actually takes in a path
bool filesys_create (const char *path, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  char name[NAME_MAX+1];

  struct dir *dir = get_directory_filename(name, path, thread_current()->cwd);

  if(dir==NULL){
    return false;
  }

  if(name[0]=='\0'){
    dir_close(dir);
    return false;
  }

  bool success = (dir != NULL && free_map_allocate (1, &inode_sector) &&
                  inode_create (inode_sector, initial_size, false) &&
                  dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *filesys_open (const char *path)
{
  char name[NAME_MAX+1];
  struct dir *dir = get_directory_filename(name, path, thread_current()->cwd);
  struct inode *inode = NULL;

  if(dir==NULL){
    return NULL;
  }

  if(name[0]=='\0'){
    struct inode *dir_inode=dir_get_inode(dir);
    inode=inode_reopen(dir_inode);
    dir_close(dir);
    return file_open(inode);
  }

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove (const char *path)
{
  char name[NAME_MAX+1];
  struct dir *dir = get_directory_filename(name, path, thread_current()->cwd);

  if(dir==NULL){
    return false;
  }
  if(name[0]=='\0'){
    dir_close(dir);
    return false;
  }
  
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir);

  return success;
}

bool filesys_chdir (const char *path)
{
  char name[NAME_MAX + 1];
  struct dir *dir = get_directory_filename(name,path,thread_current()->cwd);

  struct thread *cur=thread_current();
  struct dir *new_dir=NULL;

  // if already dir
  if (name[0]=='\0')
    {
      new_dir=dir;         
    }
  else
    {
      struct inode *inode=NULL;
      if(dir_lookup(dir,name,&inode)){
        dir_close(dir);
        new_dir=dir_open(inode);
      }else{
        dir_close(dir);
        return false;
      }
    }

  dir_close(cur->cwd);
  cur->cwd= new_dir;
  return true;
}

bool filesys_mkdir (const char *path){
  block_sector_t inode_sector=0;
  char name[NAME_MAX+1];

  struct dir *dir=get_directory_filename(name,path,thread_current()->cwd);
  if(dir==NULL){
    return false;
  }
  if(name[0]=='\0'){
      dir_close(dir);
      return false;
    }

  free_map_allocate(1,&inode_sector);
  struct inode *parent_inode=dir_get_inode(dir);
  block_sector_t parent_sector=inode_get_inumber(parent_inode);
  if(!(dir_create(inode_sector,0,parent_sector))||!(dir_add(dir,name,inode_sector))){
      dir_close(dir);
      free_map_release(inode_sector,1);
      return false;
  }
  dir_close(dir);
  return true;
}

/* Formats the file system. */
static void do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

// helper method that gets the directory and filename of any path
static struct dir *get_directory_filename(char *filename, const char *path, struct dir *cwd){

    if(path==NULL||path[0]=='\0'){
      return NULL;
    }
    
    char *path_copy=malloc(strlen(path)+1);
    strlcpy(path_copy,path,strlen(path)+1);

    char *save_ptr;
    char *token;
    char *next_token=NULL;
    
    struct dir *cur_dir;
    if(path[0]=='/'){ 
      cur_dir=dir_open_root();
    }else{
      cur_dir=dir_reopen(cwd);
    }

    token=strtok_r(path_copy,"/",&save_ptr);
    //maybe case with only "/"
    if(token==NULL){
      filename[0]='\0';
      free(path_copy);
      return cur_dir;
    }
    while(token){
      next_token=strtok_r(NULL,"/",&save_ptr);
      bool end_slash=(path[strlen(path)-1]=='/'); // case for like bruh/ which should be a dir
      if(next_token==NULL && !end_slash){
        if(strlen(token)>NAME_MAX){
          dir_close(cur_dir);
          free(path_copy);
          return NULL;
        }
        strlcpy(filename,token,NAME_MAX+1);
        break;
      }
      // we need to check if its . or .. or regular 
      if(strcmp(token,".")==0){
        // do nothing since same directory
      }else if(strcmp(token,"..")==0){
        // go back to the parent directory
        struct inode *parent_inode;
        dir_lookup(cur_dir,"..",&parent_inode);
        dir_close(cur_dir);
        cur_dir=dir_open(parent_inode);
      }else{
        struct inode *sub;
        if(!dir_lookup(cur_dir, token,&sub)){
          dir_close(cur_dir);
          free(path_copy);
          return NULL;
        }
        if(!inode_is_dir(sub)){
          inode_close(sub);
          dir_close(cur_dir);
          free(path_copy);
          return NULL;
        }
        dir_close(cur_dir);
        cur_dir = dir_open(sub);  
      }
      if(next_token==NULL && end_slash){
        filename[0]='\0';
        break;
      }

      token=next_token;

    }
    free(path_copy);
    return cur_dir;
  

}
