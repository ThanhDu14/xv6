// Tests for the sysinfo() system call.
//
// Every test calls sysinfo() before and after changing the state of
// the system (open/dup/close/pipe/fork/exit/wait ...) and checks
// that the reported open files and processes change as expected.

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

// enum procstate in kernel/proc.h
#define UNUSED   0
#define USED     1
#define SLEEPING 2
#define RUNNABLE 3
#define RUNNING  4
#define ZOMBIE   5

// struct sysinfo is ~40KB: too big for the user stack, keep it global.
static struct sysinfo si;
static struct sysinfo si0;   // a saved "before" snapshot

static int failed = 0;
static int mypid;

#define CHECK(cond, ...) do {                                  \
    if(!(cond)){                                               \
      printf("  FAIL (line %d): ", __LINE__);                  \
      printf(__VA_ARGS__);                                     \
      printf("\n");                                            \
      failed++;                                                \
    }                                                          \
  } while(0)

static char *
statename(int s)
{
  static char *names[] = { "unused", "used", "sleep", "runble", "run", "zombie" };
  if(s >= 0 && s < 6)
    return names[s];
  return "???";
}

static char *
typename(int t)
{
  static char *names[] = { "none", "pipe", "inode", "device" };
  if(t >= 0 && t < 4)
    return names[t];
  return "???";
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
getinfo(struct sysinfo *s)
{
  if(sysinfo(s) < 0){
    printf("sysinfo failed\n");
    exit(1);
  }
}

static struct procinfo *
findproc(struct sysinfo *s, int pid)
{
  for(int i = 0; i < s->nproc; i++)
    if(s->procs[i].pid == pid)
      return &s->procs[i];
  return 0;
}

static struct fileinfo *
findfile(struct sysinfo *s, int idx)
{
  for(int i = 0; i < s->nfile; i++)
    if(s->files[i].idx == idx)
      return &s->files[i];
  return 0;
}

// ftable index of fd in process pid, or -1.
static int
fdindex(struct sysinfo *s, int pid, int fd)
{
  struct procinfo *p = findproc(s, pid);
  if(p == 0)
    return -1;
  for(int i = 0; i < p->nofile; i++)
    if(p->fds[i] == fd)
      return p->fileidx[i];
  return -1;
}

// The open file of fd in this process.
static struct fileinfo *
myfile(struct sysinfo *s, int fd)
{
  int idx = fdindex(s, mypid, fd);
  return idx < 0 ? 0 : findfile(s, idx);
}

static int
haspid(struct fileinfo *f, int pid)
{
  for(int i = 0; i < f->npid; i++)
    if(f->pids[i] == pid)
      return 1;
  return 0;
}

static void
dump(struct sysinfo *s)
{
  printf("  open files: %d\n", s->nfile);
  printf("    idx type   mode ref inum itype off  pids\n");
  for(int i = 0; i < s->nfile; i++){
    struct fileinfo *f = &s->files[i];
    printf("    %d\t%s\t%s%s   %d   %d   %s\t%d   ",
           f->idx, typename(f->type),
           f->readable ? "r" : "-", f->writable ? "w" : "-",
           f->ref, f->inum, itypename(f->itype), f->off);
    for(int j = 0; j < f->npid; j++)
      printf("%d ", f->pids[j]);
    printf("\n");
  }
  printf("  processes: %d\n", s->nproc);
  printf("    pid ppid state  nsched name        fd->ftable\n");
  for(int i = 0; i < s->nproc; i++){
    struct procinfo *p = &s->procs[i];
    printf("    %d   %d    %s\t%d\t%s\t", p->pid, p->ppid,
           statename(p->state), p->nsched, p->name);
    for(int j = 0; j < p->nofile; j++)
      printf("%d->%d ", p->fds[j], p->fileidx[j]);
    printf("\n");
  }
}

// Invariants that must hold for any snapshot.
static void
checkconsistent(struct sysinfo *s)
{
  CHECK(s->nfile >= 0 && s->nfile <= NFILE, "nfile=%d", s->nfile);
  CHECK(s->nproc >= 1 && s->nproc <= NPROC, "nproc=%d", s->nproc);

  for(int i = 0; i < s->nfile; i++){
    struct fileinfo *f = &s->files[i];
    CHECK(f->ref > 0, "file %d ref=%d", f->idx, f->ref);
    CHECK(f->type == SI_FD_PIPE || f->type == SI_FD_INODE ||
          f->type == SI_FD_DEVICE, "file %d type=%d", f->idx, f->type);
    CHECK(f->readable || f->writable, "file %d has no mode", f->idx);
    // every fd is one reference; the snapshot is taken under
    // ftable.lock so it can never see more fds than ref.
    CHECK(f->nfd <= f->ref, "file %d nfd=%d > ref=%d", f->idx, f->nfd, f->ref);
    CHECK(f->npid <= f->nfd, "file %d npid=%d > nfd=%d", f->idx, f->npid, f->nfd);
  }

  for(int i = 0; i < s->nproc; i++){
    struct procinfo *p = &s->procs[i];
    CHECK(p->state >= USED && p->state <= ZOMBIE, "pid %d state=%d", p->pid, p->state);
    CHECK(p->nofile >= 0 && p->nofile <= NOFILE, "pid %d nofile=%d", p->pid, p->nofile);
    if(p->state == ZOMBIE)
      CHECK(p->nofile == 0, "zombie %d still has %d files", p->pid, p->nofile);
    for(int j = 0; j < p->nofile; j++){
      struct fileinfo *f = findfile(s, p->fileidx[j]);
      CHECK(f != 0, "pid %d fd %d -> ftable %d not listed", p->pid, p->fds[j], p->fileidx[j]);
      if(f)
        CHECK(haspid(f, p->pid), "file %d does not list pid %d", f->idx, p->pid);
    }
  }
}

static void
begin(char *name)
{
  printf("test %s:\n", name);
}

static void
end(int before)
{
  if(failed == before)
    printf("  OK\n");
}

// ---------------------------------------------------------------

void
test_basic(void)
{
  int f0 = failed;
  begin("basic");

  CHECK(sysinfo(&si) == 0, "sysinfo returned error");
  dump(&si);
  checkconsistent(&si);

  // init, sh, sysinfotest at least.
  CHECK(si.nproc >= 3, "nproc=%d", si.nproc);

  struct procinfo *me = findproc(&si, mypid);
  CHECK(me != 0, "own process not listed");
  if(me){
    CHECK(strcmp(me->name, "sysinfotest") == 0, "name=%s", me->name);
    CHECK(me->state == RUNNING, "own state=%d, want RUNNING", me->state);
    CHECK(me->nsched >= 1, "own nsched=%d", me->nsched);
    struct procinfo *parent = findproc(&si, me->ppid);
    CHECK(parent != 0, "parent %d not listed", me->ppid);
    if(parent)
      CHECK(strcmp(parent->name, "sh") == 0, "parent name=%s", parent->name);
  }

  struct procinfo *init = findproc(&si, 1);
  CHECK(init != 0, "init (pid 1) not listed");
  if(init){
    CHECK(strcmp(init->name, "init") == 0, "pid 1 name=%s", init->name);
    CHECK(init->ppid == 0, "init ppid=%d", init->ppid);
    CHECK(init->state == SLEEPING, "init state=%d", init->state);
  }

  // fd 0, 1, 2 are all the same console file (init: open + dup + dup).
  struct fileinfo *c0 = myfile(&si, 0), *c1 = myfile(&si, 1), *c2 = myfile(&si, 2);
  CHECK(c0 && c0 == c1 && c1 == c2, "fd 0/1/2 are not one shared file");
  if(c0){
    CHECK(c0->type == SI_FD_DEVICE, "console type=%d", c0->type);
    CHECK(c0->itype == T_DEVICE, "console itype=%d", c0->itype);
    CHECK(c0->major == 1, "console major=%d", c0->major);
    CHECK(c0->readable && c0->writable, "console mode");
    CHECK(haspid(c0, 1) && haspid(c0, mypid), "console not shared by init and me");
  }
  end(f0);
}

void
test_badaddr(void)
{
  int f0 = failed;
  begin("bad addresses");
  // address 0 is the (read-only) text segment.
  CHECK(sysinfo((struct sysinfo *)0) < 0, "text address accepted");
  CHECK(sysinfo((struct sysinfo *)0xffffffffffffULL) < 0, "huge address accepted");
  // a struct that runs past the end of the heap.
  char *top = sbrk(0);
  CHECK(sysinfo((struct sysinfo *)(top - 64)) < 0, "struct past heap end accepted");
  // kernel must not have been damaged: a good call still works.
  CHECK(sysinfo(&si) == 0, "good call after bad calls failed");
  end(f0);
}

void
test_openclose(void)
{
  int f0 = failed;
  begin("open / close / modes");
  getinfo(&si0);

  int fd = open("sitest.tmp", O_CREATE | O_RDWR);
  CHECK(fd >= 0, "open failed");
  getinfo(&si);
  checkconsistent(&si);
  CHECK(si.nfile == si0.nfile + 1, "nfile %d -> %d, want +1", si0.nfile, si.nfile);
  CHECK(findproc(&si, mypid)->nofile == findproc(&si0, mypid)->nofile + 1, "nofile not +1");
  struct fileinfo *f = myfile(&si, fd);
  CHECK(f != 0, "new fd not found");
  if(f){
    CHECK(f->type == SI_FD_INODE, "type=%d", f->type);
    CHECK(f->itype == T_FILE, "itype=%d", f->itype);
    CHECK(f->readable && f->writable, "O_RDWR mode");
    CHECK(f->ref == 1 && f->nfd == 1, "ref=%d nfd=%d", f->ref, f->nfd);
    CHECK(f->npid == 1 && f->pids[0] == mypid, "pids");
    CHECK(f->off == 0, "off=%d", f->off);
  }

  // offset follows writes.
  write(fd, "0123456789", 10);
  getinfo(&si);
  f = myfile(&si, fd);
  CHECK(f && f->off == 10, "off after write=%d", f ? f->off : -1);
  int inum = f ? f->inum : -1;
  close(fd);

  getinfo(&si);
  CHECK(si.nfile == si0.nfile, "nfile after close %d, want %d", si.nfile, si0.nfile);

  // read-only and write-only modes.
  fd = open("sitest.tmp", O_RDONLY);
  getinfo(&si);
  f = myfile(&si, fd);
  CHECK(f && f->readable && !f->writable, "O_RDONLY mode");
  CHECK(f && f->inum == inum, "same file, different inum");
  close(fd);

  fd = open("sitest.tmp", O_WRONLY);
  getinfo(&si);
  f = myfile(&si, fd);
  CHECK(f && !f->readable && f->writable, "O_WRONLY mode");
  close(fd);

  // a directory.
  fd = open(".", O_RDONLY);
  getinfo(&si);
  f = myfile(&si, fd);
  CHECK(f && f->type == SI_FD_INODE && f->itype == T_DIR, "directory itype");
  close(fd);

  // opening the same file twice gives two different file entries
  // (each with ref 1) pointing to the same inode.
  int a = open("sitest.tmp", O_RDONLY);
  int b = open("sitest.tmp", O_RDONLY);
  getinfo(&si);
  struct fileinfo *fa = myfile(&si, a), *fb = myfile(&si, b);
  CHECK(si.nfile == si0.nfile + 2, "two opens: nfile %d", si.nfile);
  CHECK(fa && fb && fa != fb, "two opens share an entry");
  CHECK(fa && fb && fa->inum == fb->inum, "two opens: different inode");
  CHECK(fa && fb && fa->ref == 1 && fb->ref == 1, "two opens: ref");
  close(a);
  close(b);

  getinfo(&si);
  CHECK(si.nfile == si0.nfile, "nfile at end %d, want %d", si.nfile, si0.nfile);
  unlink("sitest.tmp");
  end(f0);
}

void
test_dup(void)
{
  int f0 = failed;
  begin("dup");
  getinfo(&si0);

  int fd = open("README", O_RDONLY);
  int fd2 = dup(fd);
  int fd3 = dup(fd);
  getinfo(&si);
  checkconsistent(&si);
  // dup does not allocate a new file entry, only a new fd.
  CHECK(si.nfile == si0.nfile + 1, "nfile %d, want %d", si.nfile, si0.nfile + 1);
  CHECK(fdindex(&si, mypid, fd) == fdindex(&si, mypid, fd2) &&
        fdindex(&si, mypid, fd) == fdindex(&si, mypid, fd3), "dup fds differ");
  struct fileinfo *f = myfile(&si, fd);
  CHECK(f && f->ref == 3 && f->nfd == 3, "ref=%d nfd=%d, want 3", f ? f->ref : -1, f ? f->nfd : -1);
  CHECK(f && f->npid == 1, "npid=%d", f ? f->npid : -1);

  // offset is shared by dup'ed fds.
  char buf[5];
  read(fd2, buf, sizeof(buf));
  getinfo(&si);
  f = myfile(&si, fd);
  CHECK(f && f->off == 5, "shared off=%d", f ? f->off : -1);

  close(fd);
  getinfo(&si);
  f = myfile(&si, fd2);
  CHECK(f && f->ref == 2, "ref after one close=%d", f ? f->ref : -1);
  CHECK(myfile(&si, fd) == 0, "closed fd still listed");
  close(fd2);
  close(fd3);
  getinfo(&si);
  CHECK(si.nfile == si0.nfile, "nfile at end %d", si.nfile);
  end(f0);
}

void
test_pipe(void)
{
  int f0 = failed;
  begin("pipe");
  getinfo(&si0);

  int p[2];
  pipe(p);
  getinfo(&si);
  checkconsistent(&si);
  CHECK(si.nfile == si0.nfile + 2, "nfile %d, want +2", si.nfile);
  struct fileinfo *r = myfile(&si, p[0]), *w = myfile(&si, p[1]);
  CHECK(r && r->type == SI_FD_PIPE && r->readable && !r->writable, "read end");
  CHECK(w && w->type == SI_FD_PIPE && !w->readable && w->writable, "write end");
  CHECK(r && r->inum == -1, "pipe has an inode");
  close(p[0]);
  close(p[1]);
  getinfo(&si);
  CHECK(si.nfile == si0.nfile, "nfile at end %d", si.nfile);
  end(f0);
}

void
test_fork(void)
{
  int f0 = failed;
  begin("fork / exit / wait");

  int fd = open("README", O_RDONLY);
  int go[2], done[2];
  pipe(go);
  pipe(done);
  getinfo(&si0);

  int pid = fork();
  if(pid == 0){
    char c;
    read(go[0], &c, 1);  // block until the parent has looked
    exit(0);
  }

  sleep(2);  // let the child block in read()
  getinfo(&si);
  checkconsistent(&si);
  dump(&si);
  CHECK(si.nproc == si0.nproc + 1, "nproc %d, want +1", si.nproc);
  CHECK(si.nfile == si0.nfile, "fork created file entries");
  struct procinfo *c = findproc(&si, pid);
  CHECK(c != 0, "child not listed");
  if(c){
    CHECK(c->ppid == mypid, "child ppid=%d", c->ppid);
    CHECK(c->state == SLEEPING, "child state=%s", statename(c->state));
    CHECK(strcmp(c->name, "sysinfotest") == 0, "child name=%s", c->name);
    CHECK(c->nsched >= 1, "child nsched=%d", c->nsched);
    CHECK(c->nofile == findproc(&si, mypid)->nofile, "child nofile=%d", c->nofile);
  }
  // child shares the same file entries: same index, ref + 1.
  int fds[] = { 0, fd, go[0], go[1], done[0], done[1] };
  for(int i = 0; i < 6; i++){
    int idx = fdindex(&si, mypid, fds[i]);
    CHECK(idx == fdindex(&si, pid, fds[i]), "fd %d: child uses other entry", fds[i]);
    struct fileinfo *now = findfile(&si, idx), *before = findfile(&si0, idx);
    CHECK(now && before && now->ref == before->ref + (fds[i] == 0 ? 3 : 1),
          "fd %d: ref %d -> %d", fds[i], before ? before->ref : -1, now ? now->ref : -1);
    CHECK(now && haspid(now, mypid) && haspid(now, pid), "fd %d: pids", fds[i]);
  }

  // let the child exit, but do not wait yet: it is a ZOMBIE that
  // has already closed all its files.
  write(go[1], "x", 1);
  sleep(2);
  getinfo(&si);
  checkconsistent(&si);
  c = findproc(&si, pid);
  CHECK(c && c->state == ZOMBIE, "child state=%s, want zombie", c ? statename(c->state) : "gone");
  CHECK(c && c->nofile == 0, "zombie nofile");
  struct fileinfo *f = myfile(&si, fd);
  CHECK(f && f->ref == 1 && f->npid == 1, "ref after child exit=%d", f ? f->ref : -1);

  wait(0);
  getinfo(&si);
  CHECK(findproc(&si, pid) == 0, "child still listed after wait");
  CHECK(si.nproc == si0.nproc, "nproc after wait %d", si.nproc);

  close(fd);
  close(go[0]); close(go[1]);
  close(done[0]); close(done[1]);
  end(f0);
}

void
test_manyprocs(void)
{
  int f0 = failed;
  enum { N = 10 };
  int pids[N];
  begin("many processes");

  int go[2];
  pipe(go);
  getinfo(&si0);
  for(int i = 0; i < N; i++){
    pids[i] = fork();
    if(pids[i] < 0){
      printf("fork failed\n");
      exit(1);
    }
    if(pids[i] == 0){
      char c;
      close(go[1]);
      read(go[0], &c, 1);  // returns 0 when the parent closes go[1]
      exit(0);
    }
  }
  sleep(2);
  getinfo(&si);
  checkconsistent(&si);
  CHECK(si.nproc == si0.nproc + N, "nproc %d, want %d", si.nproc, si0.nproc + N);
  for(int i = 0; i < N; i++){
    struct procinfo *c = findproc(&si, pids[i]);
    CHECK(c && c->ppid == mypid && c->state == SLEEPING, "child %d", pids[i]);
  }
  // every child closed its copy of go[1]: only the parent holds it,
  // while go[0] is shared by all N+1 processes.
  struct fileinfo *r = myfile(&si, go[0]), *w = myfile(&si, go[1]);
  CHECK(r && r->ref == N + 1 && r->npid == N + 1, "read end ref=%d", r ? r->ref : -1);
  CHECK(w && w->ref == 1 && w->npid == 1, "write end ref=%d", w ? w->ref : -1);

  close(go[1]);
  for(int i = 0; i < N; i++)
    wait(0);
  close(go[0]);
  getinfo(&si);
  CHECK(si.nproc == si0.nproc && si.nfile == si0.nfile - 2,
        "after wait: nproc %d nfile %d", si.nproc, si.nfile);
  end(f0);
}

void
test_nsched(void)
{
  int f0 = failed;
  begin("scheduling counter");
  getinfo(&si0);
  int before = findproc(&si0, mypid)->nsched;
  // each sleep() gives up the CPU; the scheduler must pick us again.
  for(int i = 0; i < 5; i++)
    sleep(1);
  getinfo(&si);
  int after = findproc(&si, mypid)->nsched;
  printf("  nsched %d -> %d\n", before, after);
  CHECK(after >= before + 5, "nsched %d -> %d, want +5", before, after);

  // a new process starts from 0 and is counted once it runs.
  int p[2];
  pipe(p);
  int pid = fork();
  if(pid == 0){
    char c;
    read(p[0], &c, 1);
    exit(0);
  }
  sleep(2);
  getinfo(&si);
  struct procinfo *c = findproc(&si, pid);
  CHECK(c && c->nsched >= 1 && c->nsched < after, "child nsched=%d", c ? c->nsched : -1);
  write(p[1], "x", 1);
  wait(0);
  close(p[0]);
  close(p[1]);
  end(f0);
}

void
test_fdlimit(void)
{
  int f0 = failed;
  begin("per-process fd limit (NOFILE)");
  int fds[NOFILE];
  int n = 0, fd;
  while((fd = open("README", O_RDONLY)) >= 0)
    fds[n++] = fd;
  getinfo(&si);
  checkconsistent(&si);
  struct procinfo *me = findproc(&si, mypid);
  CHECK(me->nofile == NOFILE, "nofile=%d, want %d", me->nofile, NOFILE);
  for(int i = 0; i < n; i++)
    close(fds[i]);
  end(f0);
}

void
test_ftablefull(void)
{
  int f0 = failed;
  // each child already has 5 fds (console x3, ready[1], go[0]),
  // so it can open 11 more: 12 * 11 > NFILE.
  enum { N = 12 };
  begin("system-wide file table full (NFILE)");

  int ready[2], go[2];
  pipe(ready);
  pipe(go);
  for(int i = 0; i < N; i++){
    if(fork() == 0){
      char c;
      close(ready[0]);
      close(go[1]);
      while(open("README", O_RDONLY) >= 0)
        ;
      write(ready[1], "x", 1);
      read(go[0], &c, 1);
      exit(0);
    }
  }
  for(int i = 0; i < N; i++){
    char c;
    read(ready[0], &c, 1);
  }
  getinfo(&si);
  checkconsistent(&si);
  printf("  nfile=%d (NFILE=%d)\n", si.nfile, NFILE);
  CHECK(si.nfile == NFILE, "nfile=%d, want %d", si.nfile, NFILE);
  int total = 0;
  for(int i = 0; i < si.nproc; i++)
    if(si.procs[i].ppid == mypid)
      total += si.procs[i].nofile;
  CHECK(total > 0, "children hold no files");

  close(go[1]);
  for(int i = 0; i < N; i++)
    wait(0);
  close(go[0]);
  close(ready[0]);
  close(ready[1]);
  getinfo(&si);
  CHECK(si.nfile < NFILE, "files not released: nfile=%d", si.nfile);
  end(f0);
}

int
main(int argc, char *argv[])
{
  mypid = getpid();

  test_basic();
  test_badaddr();
  test_openclose();
  test_dup();
  test_pipe();
  test_fork();
  test_manyprocs();
  test_nsched();
  test_fdlimit();
  test_ftablefull();

  if(failed){
    printf("sysinfotest: %d check(s) FAILED\n", failed);
    exit(1);
  }
  printf("sysinfotest: ALL TESTS PASSED\n");
  exit(0);
}
