#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

#define offsetof(type, member) __builtin_offsetof(type, member)

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Enable tracing for the calling process: remember the mask
// of syscall numbers whose return values should be printed.
uint64
sys_trace(void)
{
  int mask;

  argint(0, &mask);
  myproc()->tracemask = mask;
  return 0;
}

// sysinfo(struct sysinfo *info): collect the open files of the
// whole system and the processes that are not UNUSED, and copy
// them to the user's struct sysinfo. Return 0, or -1 on a bad
// pointer.
uint64
sys_sysinfo(void)
{
  uint64 addr;
  int nfile, nproc;
  struct proc *p = myproc();

  argaddr(0, &addr);

  nfile = filesysinfo(addr + offsetof(struct sysinfo, files));
  if(nfile < 0)
    return -1;
  nproc = procsysinfo(addr + offsetof(struct sysinfo, procs));
  if(nproc < 0)
    return -1;

  if(copyout(p->pagetable, addr + offsetof(struct sysinfo, nfile),
             (char *)&nfile, sizeof(nfile)) < 0 ||
     copyout(p->pagetable, addr + offsetof(struct sysinfo, nproc),
             (char *)&nproc, sizeof(nproc)) < 0)
    return -1;
  return 0;
}
