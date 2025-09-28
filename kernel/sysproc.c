#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int options;
  if (argaddr(0, &p) < 0) return -1;
  if (argint(1, &options) < 0) return -1;
  return wait(p, options);
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

uint64 sys_yield(void) {
  struct proc *p = myproc();
  
  // 打印当前进程的内核线程上下文被保存的地址范围
  printf("Save the context of the process to the memory region from address %p to %p\n", 
         &p->context, (char*)&p->context + sizeof(struct context));
  
  // 打印当前进程的pid和用户态pc值
  printf("Current running process pid is %d and user pc is %p\n", 
         p->pid, p->trapframe->epc);
  
  // 找到下一个可运行的进程（从当前进程的下一个位置开始，实现Round-Robin）
  struct proc *next = 0;
  struct proc *start = p + 1;
  if (start >= &proc[NPROC]) {
    start = proc;
  }
  
  for (struct proc *np = start; np != p; np++) {
    if (np >= &proc[NPROC]) {
      np = proc;
    }
    acquire(&np->lock);
    if (np->state == RUNNABLE) {
      next = np;
      break;
    }
    release(&np->lock);
  }
  
  if (next) {
    // 打印即将被调度到的进程的pid和用户态pc值
    printf("Next runnable process pid is %d and user pc is %p\n", 
           next->pid, next->trapframe->epc);
    release(&next->lock);
  } else {
    printf("Next runnable process pid is -1 and user pc is 0x0\n");
  }
  
  // 让出CPU
  yield();
  
  return 0;
}