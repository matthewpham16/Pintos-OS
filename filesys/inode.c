#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIR_CNT 123
#define PTRS_PER_SECTOR 128
#define MAX_FILE_SIZE 8388608 //8 MB

struct lock open_inodes_lock;

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  off_t length;        
  unsigned magic;       
  block_sector_t direct[DIR_CNT]; // u32
  block_sector_t singly_indirect;
  block_sector_t doubly_indirect;
  bool is_directory;
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
  struct lock inode_lock;
};

//inode helper methods
bool inode_is_dir(struct inode *inode){
  return inode->data.is_directory;
}

bool inode_is_root(struct inode *inode){
  return inode->sector==ROOT_DIR_SECTOR;
}

bool inode_in_use(struct inode *inode){
  lock_acquire(&inode->inode_lock);
  bool in_use = inode->open_cnt>1;
  lock_release(&inode->inode_lock);
  return in_use;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length) {
    off_t idx = pos / BLOCK_SECTOR_SIZE;

    if (idx < DIR_CNT) {
      return inode->data.direct[idx] != NULL ? inode->data.direct[idx] : -1;
    }

    idx -= DIR_CNT;

    if (idx < PTRS_PER_SECTOR) {
      if (inode->data.singly_indirect == NULL) {
        return -1;
      }

      block_sector_t* indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
      if (indirect_block == NULL) {
        return -1;
      }
      // this has internal synchronization
      block_read(fs_device, inode->data.singly_indirect, indirect_block);
      block_sector_t res = indirect_block[idx];
      free(indirect_block);
      return res == 0 ? -1 : res;
    }
    idx -= PTRS_PER_SECTOR;
    
    if (idx < PTRS_PER_SECTOR * PTRS_PER_SECTOR) {
      if (inode->data.doubly_indirect == NULL)
        return -1;
      
      block_sector_t* doubly_indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
      if (doubly_indirect_block == NULL) {
        return -1;
      }
      block_read(fs_device, inode->data.doubly_indirect, doubly_indirect_block);
      off_t doubly_indirect_idx = idx / PTRS_PER_SECTOR;
      off_t indirect_idx = idx % PTRS_PER_SECTOR;

      if (doubly_indirect_block[doubly_indirect_idx] == NULL) {
        free(doubly_indirect_block);
        return -1;
      }
      
      block_sector_t* indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
      if (indirect_block == NULL) {
        free(doubly_indirect_block);
        return -1;
      }
      block_read(fs_device, doubly_indirect_block[doubly_indirect_idx], indirect_block);
      block_sector_t res = indirect_block[indirect_idx];
      free(doubly_indirect_block);
      free(indirect_block);
      return res == 0 ? -1 : res;
    }
  }
  return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init (void) { 
  list_init (&open_inodes);
  lock_init(&open_inodes_lock);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create (block_sector_t sector, off_t length, bool directory)
{
  // Lets assume sector param is already secured
  // idea -- allocate on disk -- then write

  bool success = false;
  ASSERT (length >= 0);
  if (length > MAX_FILE_SIZE)
    return success;

  struct inode_disk* disk_inode = calloc(1, sizeof(struct inode_disk));
  if (disk_inode == NULL) {
    return success;
  }
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);
  int sectors = bytes_to_sectors(length);
  int sectors_it = sectors;


  for (size_t idx = 0; idx < DIR_CNT && idx < sectors_it; idx++) {
    if (free_map_allocate(1, &disk_inode->direct[idx]) == false) {
      free_sectors(disk_inode);
      free(disk_inode);
      return success;
    }
  }

  sectors_it -= DIR_CNT;
  block_sector_t* indirect_block = 0;
  if (sectors_it > 0) {
    indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
    if (indirect_block == NULL) {
      free_sectors(disk_inode);
      free(disk_inode);
      return success;
    }

    if (free_map_allocate(1, &disk_inode->singly_indirect) == false) {
      free_sectors(disk_inode);
      free(indirect_block);
      free(disk_inode);
      return success;
    }

    for (size_t idx = 0; idx < PTRS_PER_SECTOR && idx < sectors_it; idx++) {
      if (free_map_allocate(1, &indirect_block[idx]) == false) {
        block_write(fs_device, disk_inode->singly_indirect, indirect_block);
        free_sectors(disk_inode);
        free(indirect_block);
        free(disk_inode);
        return success;
      }
    }
    block_write(fs_device, disk_inode->singly_indirect, indirect_block);
  }


  sectors_it -= PTRS_PER_SECTOR;

  if (sectors_it > 0) {
    block_sector_t* doubly_indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
    if (doubly_indirect_block == NULL) {
      free_sectors(disk_inode);
      free(indirect_block);
      free(disk_inode);
      return success;
    }

    if (free_map_allocate(1, &disk_inode->doubly_indirect) == false) {
      free_sectors(disk_inode);
      free(doubly_indirect_block);
      free(indirect_block);
      free(disk_inode);
      return success;
    }

    for (size_t i = 0; i < PTRS_PER_SECTOR && sectors_it > 0; i++) {

      // We have a singly indirect space and a doubly indirect space
      if (free_map_allocate(1, &doubly_indirect_block[i]) == false) {
        block_write(fs_device, disk_inode->doubly_indirect, doubly_indirect_block);
        free_sectors(disk_inode);
        free(doubly_indirect_block);
        free(indirect_block);
        free(disk_inode);
        return success;
      }

      memset(indirect_block, 0, PTRS_PER_SECTOR * sizeof(block_sector_t));
      for (size_t j = 0; j < PTRS_PER_SECTOR && sectors_it > 0; j++) {
        if (free_map_allocate(1, &indirect_block[j]) == false) {
          block_write(fs_device, doubly_indirect_block[i], indirect_block);
          block_write(fs_device, disk_inode->doubly_indirect, doubly_indirect_block);
          free_sectors(disk_inode);
          free(doubly_indirect_block);
          free(indirect_block);
          free(disk_inode);
          return success;
        }
        sectors_it--;
      }

      block_write(fs_device, doubly_indirect_block[i], indirect_block);
    }
    block_write(fs_device, disk_inode->doubly_indirect, doubly_indirect_block);
    free(doubly_indirect_block);
    free(indirect_block);
  } else {
    if (indirect_block != 0) {
      free(indirect_block);
    }
  }

  zero_sectors(disk_inode);
  disk_inode->length = length;
  disk_inode->magic = INODE_MAGIC;
  disk_inode->is_directory=directory;
  block_write(fs_device, sector, disk_inode) ;
  free(disk_inode);
  success = true;
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  lock_acquire(&open_inodes_lock);

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          lock_release(&open_inodes_lock);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL) {
    lock_release(&open_inodes_lock);
    return NULL;
  }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->inode_lock);
  block_read (fs_device, inode->sector, &inode->data);

  lock_release(&open_inodes_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen (struct inode *inode)
{
  if (inode != NULL) {
    lock_acquire(&inode->inode_lock);
    inode->open_cnt++;
    lock_release(&inode->inode_lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk. (Does it?  Check code.)
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire(&open_inodes_lock);
  lock_acquire(&inode->inode_lock);

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
      lock_release(&open_inodes_lock);

      // write inode data back to disk before closing
      block_write (fs_device, inode->sector, &inode->data);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          free_map_release (inode->sector, 1);
          free_sectors (&inode->data);
        }

      lock_release(&inode->inode_lock);
      free (inode);
    }
  else {
    lock_release(&inode->inode_lock);
    lock_release(&open_inodes_lock);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  lock_acquire(&inode->inode_lock);
  inode->removed = true;
  lock_release(&inode->inode_lock);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at (struct inode *inode, void *buffer_, off_t size,
                     off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  // Prevents reading past end of file.
  off_t reading_len = offset + size;
  lock_acquire(&inode->inode_lock);
  if (reading_len > inode->data.length) {
    off_t diff = reading_len - inode->data.length;
    size -= diff;
  }
  lock_release(&inode->inode_lock);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0 || offset + chunk_size >= MAX_FILE_SIZE) // TODO consider edge cases
        break;
      
      if (sector_idx == (block_sector_t) -1) {
        memset(buffer + bytes_read, 0, chunk_size);
      }
      else if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                      off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  lock_acquire(&inode->inode_lock);
  if (inode->deny_write_cnt) {
    lock_release(&inode->inode_lock);
    return 0;
  }
  lock_release(&inode->inode_lock);

  // if (offset + size > MAX_FILE_SIZE)
  //   return 0;

  bool extending = 0;
  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector*/
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size;

      // still inside file
      if (inode_left > 0)
        {
          int min_left = inode_left < sector_left ? inode_left : sector_left;
          chunk_size = size < min_left ? size : min_left;
        }
      else //beyond file, past or at EOF, have to extend
        {
          if (extending == 0)
            lock_acquire(&inode->inode_lock);
          extending = 1;
          chunk_size = size < sector_left ? size : sector_left;
        }

      if (offset + chunk_size > MAX_FILE_SIZE)
        { 
          chunk_size = MAX_FILE_SIZE - offset; //write as much as possible
        }

      block_sector_t sector_idx = byte_to_sector_allocate (inode, offset);
      if (sector_idx == (block_sector_t) -1){
          break;  
      }

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector first.
             Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  inode->data.length = offset > inode->data.length ? offset : inode->data.length;
  block_write (fs_device, inode->sector, &inode->data);
  if (extending)
    lock_release(&inode->inode_lock);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write (struct inode *inode)
{
  lock_acquire(&inode->inode_lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release(&inode->inode_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (struct inode *inode)
{
  lock_acquire(&inode->inode_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release(&inode->inode_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length (const struct inode *inode) { return inode->data.length; }

// Frees everything BUT the inode itself
void free_sectors(struct inode_disk *inode_disk) {

  // Direct
  for (off_t idx = 0; idx < DIR_CNT; idx++) {
    if (inode_disk->direct[idx] == NULL) {
      continue;
    }
    free_map_release(inode_disk->direct[idx], 1);
    inode_disk->direct[idx] = NULL;
  }


  block_sector_t* doubly_indirect_block = malloc(PTRS_PER_SECTOR * sizeof(block_sector_t));
  if (doubly_indirect_block == NULL) {
    PANIC ("Unable to free inode successfully");
  }

  block_sector_t* indirect_block = malloc(PTRS_PER_SECTOR * sizeof(block_sector_t));
  if (indirect_block == NULL) {
      PANIC ("Unable to free inode successfully");
  }

  if (inode_disk->singly_indirect != NULL) {
    // Indirect
    block_read(fs_device, inode_disk->singly_indirect, indirect_block);

    for (off_t idx = 0; idx < PTRS_PER_SECTOR; idx++) {
      if (indirect_block[idx] == NULL) {
        continue;
      }
      // Free data
      free_map_release(indirect_block[idx], 1);
      indirect_block[idx] = NULL;
    }
    free_map_release(inode_disk->singly_indirect, 1);
  }

  if (inode_disk->doubly_indirect != NULL) {
    block_read(fs_device, inode_disk->doubly_indirect, doubly_indirect_block);
    for (off_t i = 0; i < PTRS_PER_SECTOR; i++) { // Outer layer
      if (doubly_indirect_block[i] == NULL) {
        continue;
      }

      block_read(fs_device, doubly_indirect_block[i], indirect_block);
      for (off_t j = 0; j < PTRS_PER_SECTOR; j++) { // Inner layer
        if (indirect_block[j] == NULL) {
          continue;
        }

        // Frees data sector
        free_map_release(indirect_block[j], 1);
        indirect_block[j] = NULL;
      }

      // Free indirect block
      free_map_release(doubly_indirect_block[i], 1);
      doubly_indirect_block[i] = NULL;
    }

    // Free 2x indirect block
    free_map_release(inode_disk->doubly_indirect, 1);
    inode_disk->doubly_indirect = NULL;
  }
  free(doubly_indirect_block);
  free(indirect_block);
}

/* Zeroes out sectors */
void zero_sectors(struct inode_disk *inode_disk) {

  static char zeroes[BLOCK_SECTOR_SIZE];
  // Direct
  for (off_t idx = 0; idx < DIR_CNT; idx++) {
    if (inode_disk->direct[idx] == NULL) {
      continue;
    }
    // Data
    block_write(fs_device, inode_disk->direct[idx], zeroes);
  }


  block_sector_t* doubly_indirect_block = malloc(PTRS_PER_SECTOR * sizeof(block_sector_t));
  if (doubly_indirect_block == NULL) {
    PANIC ("Unable to free inode successfully");
  }

  block_sector_t* indirect_block = malloc(PTRS_PER_SECTOR * sizeof(block_sector_t));
  if (indirect_block == NULL) {
      PANIC ("Unable to free inode successfully");
  }

  if (inode_disk->singly_indirect != NULL) {
    // Indirect
    block_read(fs_device, inode_disk->singly_indirect, indirect_block);

    for (off_t idx = 0; idx < PTRS_PER_SECTOR; idx++) {
      if (indirect_block[idx] == NULL) {
        continue;
      }
      // Free data
      block_write(fs_device, indirect_block[idx], zeroes);
    }
  }

  if (inode_disk->doubly_indirect != NULL) {
    block_read(fs_device, inode_disk->doubly_indirect, doubly_indirect_block);
    for (off_t i = 0; i < PTRS_PER_SECTOR; i++) { // Outer layer
      if (doubly_indirect_block[i] == NULL) {
        continue;
      }

      block_read(fs_device, doubly_indirect_block[i], indirect_block);
      for (off_t j = 0; j < PTRS_PER_SECTOR; j++) { // Inner layer
        if (indirect_block[j] == NULL) {
          continue;
        }

        block_write(fs_device, indirect_block[j], zeroes);
      }

    }
  }
  free(doubly_indirect_block);
  free(indirect_block);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
block_sector_t byte_to_sector_allocate (struct inode *inode, off_t pos)
{

  static char zeroes[BLOCK_SECTOR_SIZE];

  // TODO
  ASSERT (inode != NULL);
  // if (pos < inode->data.length) {
    off_t idx = pos / BLOCK_SECTOR_SIZE;

    if (idx < DIR_CNT) {
      if (inode->data.direct[idx] == NULL) {
        if (free_map_allocate(1, &inode->data.direct[idx]) == NULL) {
          return -1;
        }
        block_write(fs_device, inode->data.direct[idx], zeroes);
      }
      return inode->data.direct[idx];
    }

    idx -= DIR_CNT;

    if (idx < PTRS_PER_SECTOR) {
      if (inode->data.singly_indirect == NULL) {
        if (free_map_allocate(1, &inode->data.singly_indirect) == NULL) {
          return -1;
        }
        block_write(fs_device, inode->data.singly_indirect, zeroes);
      }

      block_sector_t* indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
      if (indirect_block == NULL) {
        return -1;
      }
      block_read(fs_device, inode->data.singly_indirect, indirect_block);
      if (indirect_block[idx] == 0) {
        if (free_map_allocate(1, &indirect_block[idx]) == NULL) {
          free(indirect_block);
          return -1;
        }
        block_write(fs_device, indirect_block[idx], zeroes);
        block_write(fs_device, inode->data.singly_indirect, indirect_block);
      }
      block_sector_t res = indirect_block[idx];
      free(indirect_block);
      return res;
    }
    idx -= PTRS_PER_SECTOR;
    
    if (idx < PTRS_PER_SECTOR * PTRS_PER_SECTOR) {
      if (inode->data.doubly_indirect == NULL) {
        if (free_map_allocate(1, &inode->data.doubly_indirect) == NULL) {
          return -1;
        }
        block_write(fs_device, inode->data.doubly_indirect, zeroes);
      }
      
      block_sector_t* doubly_indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
      if (doubly_indirect_block == NULL) {
        return -1;
      }

      block_read(fs_device, inode->data.doubly_indirect, doubly_indirect_block);
      off_t doubly_indirect_idx = idx / PTRS_PER_SECTOR;
      off_t indirect_idx = idx % PTRS_PER_SECTOR;

      if (doubly_indirect_block[doubly_indirect_idx] == NULL) {
        if (free_map_allocate(1, &doubly_indirect_block[doubly_indirect_idx]) == NULL) {
          free(doubly_indirect_block);
          return -1;
        }
        block_write(fs_device, doubly_indirect_block[doubly_indirect_idx], zeroes);
        block_write(fs_device, inode->data.doubly_indirect, doubly_indirect_block);
      }
      
      block_sector_t* indirect_block = calloc(PTRS_PER_SECTOR, sizeof(block_sector_t));
      if (indirect_block == NULL) {
        free(doubly_indirect_block);
        return -1;
      }

      block_read(fs_device, doubly_indirect_block[doubly_indirect_idx], indirect_block);
      if (indirect_block[indirect_idx] == 0) {
        if (free_map_allocate(1, &indirect_block[indirect_idx]) == 0) {
          free(doubly_indirect_block);
          free(indirect_block);
          return -1;
        }
        block_write(fs_device, indirect_block[indirect_idx], zeroes);
        block_write(fs_device, doubly_indirect_block[doubly_indirect_idx], indirect_block);
      }
      block_sector_t res = indirect_block[indirect_idx];
      free(doubly_indirect_block);
      free(indirect_block);
      return res;
    }
  // }
  return -1;
}