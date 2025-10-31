// 缓冲区缓存（Buffer cache）。
//
// 缓冲区缓存是由 buf 结构组成的双向链表，保存了磁盘块内容
// 的缓存副本。将磁盘块缓存在内存中可以减少磁盘读写次数，
// 同时为多个进程共享同一磁盘块提供同步点。
//
// 接口说明：
// * 获取某个磁盘块的缓冲区，调用 bread。
// * 修改缓冲区数据后，调用 bwrite 将其写回磁盘。
// * 使用完缓冲区后，调用 brelse 释放。
// * 调用 brelse 后不得再使用该缓冲区。
// * 同一时刻仅允许一个进程使用一个缓冲区，
//   因此不要长时间持有。


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
  
  // 每个桶对应的哈希链表哨兵
  struct buf hashbucket[NBUCKETS];
} bcache;

// 哈希函数：确定某块 (dev, blockno) 属于哪个桶
static uint hash(uint dev, uint blockno){
  return (dev * 31 + blockno) % NBUCKETS;
}

void binit(void) {
  struct buf *b;
  int i;

  for(i = 0; i < NBUCKETS; i++) {
    static char lock_name[20];
    snprintf(lock_name, sizeof(lock_name), "bcache%d", i);
    initlock(&bcache.lock[i], lock_name);
    
    bcache.hashbucket[i].prev = &bcache.hashbucket[i];
    bcache.hashbucket[i].next = &bcache.hashbucket[i];
  }

  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
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
static struct buf* bget(uint dev, uint blockno) {
  struct buf *b;
  uint bucket = hash(dev, blockno);

  // 先检查目标桶中是否已缓存该块
  acquire(&bcache.lock[bucket]);

  for(b = bcache.hashbucket[bucket].next; b != &bcache.hashbucket[bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 目标桶未命中。需要寻找空闲缓冲。
  // 优先在目标桶中尝试查找空闲缓冲
  for(b = bcache.hashbucket[bucket].next; b != &bcache.hashbucket[bucket]; b = b->next){
    if(b->refcnt == 0) {
      // 在同一桶中找到空闲缓冲
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 目标桶没有空闲缓冲，需要搜索其他桶
  release(&bcache.lock[bucket]);
  
  // 在其他桶中搜索空闲缓冲
  for(int i = 0; i < NBUCKETS; i++) {
    if(i == bucket) continue;

    acquire(&bcache.lock[i]);
    for(b = bcache.hashbucket[i].next; b != &bcache.hashbucket[i]; b = b->next){
      if(b->refcnt == 0) {
        // 将一个空闲缓冲从桶 i 迁移到目标桶 bucket。
        int first = i < (int)bucket ? i : (int)bucket;
        int second = i < (int)bucket ? (int)bucket : i;

        if(first == i) {
          // 已持有较小编号的锁 i（first）；接着获取较大编号的锁（second）。
          acquire(&bcache.lock[second]);

          // 优先检查目标桶是否已存在 (dev, blockno)，避免重复缓冲
          struct buf *t;
          for(t = bcache.hashbucket[bucket].next; t != &bcache.hashbucket[bucket]; t = t->next){
            if(t->dev == dev && t->blockno == blockno){
              t->refcnt++;
              release(&bcache.lock[second]);
              release(&bcache.lock[i]);
              acquiresleep(&t->lock);
              return t;
            }
          }

          // 在双锁到位后复验 b 仍为空闲。
          if(b->refcnt != 0) {
            release(&bcache.lock[second]);
            continue;
          }

          b->next->prev = b->prev;
          b->prev->next = b->next;

          b->dev = dev;
          b->blockno = blockno;
          b->valid = 0;
          b->refcnt = 1;
          b->next = bcache.hashbucket[bucket].next;
          b->prev = &bcache.hashbucket[bucket];
          bcache.hashbucket[bucket].next->prev = b;
          bcache.hashbucket[bucket].next = b;

          // 以相反顺序释放两把锁
          release(&bcache.lock[second]);
          release(&bcache.lock[i]);

          acquiresleep(&b->lock);
          return b;
        } else {
          // 需要先获取 bucket 再获取 i。先释放 i，按序重新获取。
          release(&bcache.lock[i]);
          acquire(&bcache.lock[first]);   // bucket
          acquire(&bcache.lock[second]);  // i

          // 双锁到位后，先检查目标桶是否已存在 (dev, blockno)
          struct buf *t2;
          for(t2 = bcache.hashbucket[bucket].next; t2 != &bcache.hashbucket[bucket]; t2 = t2->next){
            if(t2->dev == dev && t2->blockno == blockno){
              t2->refcnt++;
              release(&bcache.lock[second]);
              release(&bcache.lock[first]);
              acquiresleep(&t2->lock);
              return t2;
            }
          }

          // 在新锁序下重新扫描桶 i 的空闲，因为 b 可能已失效。
          struct buf *b2;
          for(b2 = bcache.hashbucket[i].next; b2 != &bcache.hashbucket[i]; b2 = b2->next){
            if(b2->refcnt == 0) {
              // 将 b2 从 i 迁移到 bucket（先摘除，再头插）
              b2->next->prev = b2->prev;
              b2->prev->next = b2->next;
              b2->dev = dev;
              b2->blockno = blockno;
              b2->valid = 0;
              b2->refcnt = 1;
              b2->next = bcache.hashbucket[bucket].next;
              b2->prev = &bcache.hashbucket[bucket];
              bcache.hashbucket[bucket].next->prev = b2;
              bcache.hashbucket[bucket].next = b2;
              // 按相反顺序释放两把锁
              release(&bcache.lock[second]);
              release(&bcache.lock[first]);

              acquiresleep(&b2->lock);
              return b2;
            }
          }
          // 未找到空闲；释放两把锁后继续外层搜索
          release(&bcache.lock[second]);
          release(&bcache.lock[first]);
          // 重新获取 i 的锁，以便安全地继续 i 的外层循环。
          acquire(&bcache.lock[i]);
          // 重置遍历位置，避免使用可能失效的迭代器
          b = &bcache.hashbucket[i];
        }
      }
    }
    release(&bcache.lock[i]);
  }
  
  panic("bget: no buffers");
}

// 返回一个已加锁、且包含指定块内容的缓冲区
struct buf* bread(uint dev, uint blockno) {
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// 将缓冲区 b 的内容写回磁盘。调用时必须已持有 b 的睡眠锁。
void bwrite(struct buf *b) {
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// 释放一个已加锁的缓冲区。
// 将其移动到桶内“最近使用”的位置
void brelse(struct buf *b) {
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // 移动到该桶 LRU 列表的头部
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.hashbucket[bucket].next;
    b->prev = &bcache.hashbucket[bucket];
    bcache.hashbucket[bucket].next->prev = b;
    bcache.hashbucket[bucket].next = b;
  }
  
  release(&bcache.lock[bucket]);
}

void bpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt++;
  release(&bcache.lock[bucket]);
}

void bunpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno);
  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  release(&bcache.lock[bucket]);
}


