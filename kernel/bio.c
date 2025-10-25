// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13

struct {
  struct spinlock lock[NBUCKETS];
  struct buf buf[NBUF];
  
  // Hash buckets for each bucket
  struct buf hashbucket[NBUCKETS];
} bcache;

// Hash function to determine which bucket a block belongs to
static uint
hash(uint dev, uint blockno)
{
  return (dev * 31 + blockno) % NBUCKETS;
}

void
binit(void)
{
  struct buf *b;
  int i;

  // Initialize locks for each hash bucket
  for(i = 0; i < NBUCKETS; i++) {
    char lock_name[20];
    snprintf(lock_name, sizeof(lock_name), "bcache%d", i);
    initlock(&bcache.lock[i], lock_name);
    
    // Initialize hash bucket linked lists
    bcache.hashbucket[i].prev = &bcache.hashbucket[i];
    bcache.hashbucket[i].next = &bcache.hashbucket[i];
  }

  // Initialize all buffers and add them to bucket 0 initially
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.hashbucket[0].next;
    b->prev = &bcache.hashbucket[0];
    initsleeplock(&b->lock, "buffer");
    bcache.hashbucket[0].next->prev = b;
    bcache.hashbucket[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucket = hash(dev, blockno);

  // First, check if the block is already cached in the target bucket
  acquire(&bcache.lock[bucket]);
  
  // Is the block already cached?
  for(b = bcache.hashbucket[bucket].next; b != &bcache.hashbucket[bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached in target bucket. Need to find a free buffer.
  // First try to find a free buffer in the target bucket
  for(b = bcache.hashbucket[bucket].next; b != &bcache.hashbucket[bucket]; b = b->next){
    if(b->refcnt == 0) {
      // Found a free buffer in the same bucket
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // No free buffer in target bucket, need to search other buckets
  // To avoid deadlock, we need to release current lock and acquire locks in order
  release(&bcache.lock[bucket]);
  
  // Search other buckets for a free buffer
  for(int i = 0; i < NBUCKETS; i++) {
    if(i == bucket) continue; // Skip the target bucket we already checked
    
    acquire(&bcache.lock[i]);
    for(b = bcache.hashbucket[i].next; b != &bcache.hashbucket[i]; b = b->next){
      if(b->refcnt == 0) {
        // Found a free buffer, move it to target bucket
        // Remove from current bucket
        b->next->prev = b->prev;
        b->prev->next = b->next;
        
        // Add to target bucket (need to acquire target bucket lock)
        acquire(&bcache.lock[bucket]);
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        b->next = bcache.hashbucket[bucket].next;
        b->prev = &bcache.hashbucket[bucket];
        bcache.hashbucket[bucket].next->prev = b;
        bcache.hashbucket[bucket].next = b;
        release(&bcache.lock[bucket]);
        release(&bcache.lock[i]);
        
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[i]);
  }
  
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // Move to the head of the bucket's LRU list
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.hashbucket[bucket].next;
    b->prev = &bcache.hashbucket[bucket];
    bcache.hashbucket[bucket].next->prev = b;
    bcache.hashbucket[bucket].next = b;
  }
  
  release(&bcache.lock[bucket]);
}

void
bpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt++;
  release(&bcache.lock[bucket]);
}

void
bunpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  release(&bcache.lock[bucket]);
}


