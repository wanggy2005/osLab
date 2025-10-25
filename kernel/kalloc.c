// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];

void
kinit()
{
  // Initialize locks for all CPUs
  for(int i = 0; i < NCPU; i++) {
    static char lock_names[NCPU][10];
    snprintf(lock_names[i], sizeof(lock_names[i]), "kmem%d", i);
    initlock(&kmems[i].lock, lock_names[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // Get current CPU ID
  push_off();
  int cpu_id = cpuid();
  pop_off();

  acquire(&kmems[cpu_id].lock);
  r->next = kmems[cpu_id].freelist;
  kmems[cpu_id].freelist = r;
  release(&kmems[cpu_id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  
  // Get current CPU ID
  push_off();
  int cpu_id = cpuid();
  pop_off();

  // Try to allocate from current CPU's freelist first
  acquire(&kmems[cpu_id].lock);
  r = kmems[cpu_id].freelist;
  if(r)
    kmems[cpu_id].freelist = r->next;
  release(&kmems[cpu_id].lock);

  // If current CPU has no free pages, try to steal from other CPUs
  if(r == 0) {
    for(int i = 0; i < NCPU; i++) {
      if(i == cpu_id) continue; // Skip current CPU
      
      acquire(&kmems[i].lock);
      r = kmems[i].freelist;
      if(r) {
        kmems[i].freelist = r->next;
        release(&kmems[i].lock);
        break; // Found a page, exit the loop
      }
      release(&kmems[i].lock);
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

