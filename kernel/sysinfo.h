// Information returned by the sysinfo() system call.
// Requires kernel/param.h (NFILE, NPROC, NOFILE) to be included first.

// File types (same values as enum in struct file, kernel/file.h).
#define SI_FD_NONE    0
#define SI_FD_PIPE    1
#define SI_FD_INODE   2
#define SI_FD_DEVICE  3

// One open entry of the system-wide file table (ftable.file[]).
struct fileinfo {
  int idx;              // index in ftable.file[]
  int type;             // SI_FD_PIPE, SI_FD_INODE, SI_FD_DEVICE
  int readable;         // mode: opened for reading?
  int writable;         // mode: opened for writing?
  int ref;              // reference count (struct file.ref)
  int inum;             // inode number (FD_INODE / FD_DEVICE), else -1
  int itype;            // inode type T_DIR/T_FILE/T_DEVICE, else -1
  int major;            // device major number (FD_DEVICE), else -1
  uint off;             // current offset (FD_INODE)
  int nfd;              // number of fds (over all processes) pointing here
  int npid;             // number of distinct processes referencing it
  int pids[NPROC];      // pids of those processes
};

// One process whose state is not UNUSED.
struct procinfo {
  int pid;
  int state;            // enum procstate: 1 USED, 2 SLEEPING, 3 RUNNABLE,
                        //                 4 RUNNING, 5 ZOMBIE
  int ppid;             // parent's pid (0 if no parent)
  int nsched;           // number of times the scheduler picked this process
  char name[16];
  int nofile;           // number of open fds
  int fds[NOFILE];      // the fd numbers
  int fileidx[NOFILE];  // index in ftable.file[] for each fd above
};

struct sysinfo {
  int nfile;                      // number of open files in the system
  int nproc;                      // number of processes not UNUSED
  struct fileinfo files[NFILE];   // first nfile entries are valid
  struct procinfo procs[NPROC];   // first nproc entries are valid
};
