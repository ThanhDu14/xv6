// sysinfo: print the open files and the processes of the running
// system, using the sysinfo() system call (like ps + lsof).
//
//   sysinfo        print open files and processes
//   sysinfo -f     only open files
//   sysinfo -p     only processes

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

// too big (~40KB) for the user stack.
static struct sysinfo si;

static char *
statename(int s)
{
  static char *names[] = { "unused", "used", "sleep", "runble", "run", "zombie" };
  return (s >= 0 && s < 6) ? names[s] : "???";
}

static char *
typename(int t)
{
  static char *names[] = { "none", "pipe", "inode", "device" };
  return (t >= 0 && t < 4) ? names[t] : "???";
}

static char *
itypename(int t)
{
  if(t == T_DIR) return "dir";
  if(t == T_FILE) return "file";
  if(t == T_DEVICE) return "dev";
  return "-";
}

static void
printfiles(void)
{
  printf("OPEN FILES: %d\n", si.nfile);
  printf("IDX\tTYPE\tMODE\tREF\tINUM\tITYPE\tOFF\tPIDS\n");
  for(int i = 0; i < si.nfile; i++){
    struct fileinfo *f = &si.files[i];
    printf("%d\t%s\t%s%s\t%d\t", f->idx, typename(f->type),
           f->readable ? "r" : "-", f->writable ? "w" : "-", f->ref);
    if(f->inum >= 0)
      printf("%d\t%s\t%d\t", f->inum, itypename(f->itype), f->off);
    else
      printf("-\t-\t-\t");
    for(int j = 0; j < f->npid; j++)
      printf("%d ", f->pids[j]);
    printf("\n");
  }
}

static void
printprocs(void)
{
  printf("PROCESSES: %d\n", si.nproc);
  printf("PID\tPPID\tSTATE\tNSCHED\tNAME\t\tFD->FILE\n");
  for(int i = 0; i < si.nproc; i++){
    struct procinfo *p = &si.procs[i];
    printf("%d\t%d\t%s\t%d\t%s\t", p->pid, p->ppid, statename(p->state),
           p->nsched, p->name);
    if(strlen(p->name) < 8)
      printf("\t");
    for(int j = 0; j < p->nofile; j++)
      printf("%d->%d ", p->fds[j], p->fileidx[j]);
    printf("\n");
  }
}

int
main(int argc, char *argv[])
{
  int files = 1, procs = 1;

  if(argc > 1){
    if(strcmp(argv[1], "-f") == 0)
      procs = 0;
    else if(strcmp(argv[1], "-p") == 0)
      files = 0;
    else {
      fprintf(2, "usage: sysinfo [-f | -p]\n");
      exit(1);
    }
  }

  if(sysinfo(&si) < 0){
    fprintf(2, "sysinfo: system call failed\n");
    exit(1);
  }

  if(files)
    printfiles();
  if(files && procs)
    printf("\n");
  if(procs)
    printprocs();
  exit(0);
}
