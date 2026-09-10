#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

#ifdef SCHEDULER_MLFQ
// Forward declaration because MLFQ queue insertion is used
// before its full definition later in this file.
static void mlfq_enqueue(struct proc *p);
#endif

extern char trampoline[]; // trampoline.S

#ifdef SCHEDULER_MLFQ
// Forward declaration because MLFQ queue insertion is used
// before its full definition later in this file.
static void mlfq_enqueue(struct proc *p);

// Return 1 if a runnable process exists in a higher-priority
// queue than the supplied queue.
static int mlfq_higher_priority_runnable(int queue);
#endif

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

#ifdef SCHEDULER_MLFQ
// Monotonically increasing sequence number used to determine the order
// in which runnable processes entered their MLFQ queue.
uint64 mlfq_next_seq = 0;

// Protects mlfq_next_seq when multiple CPUs enqueue processes at once.
struct spinlock mlfq_seq_lock;

// Set by the timer interrupt when a 48-tick priority boost is due.
// The scheduler will perform the actual boost.
int mlfq_boost_pending = 0;
#endif

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

#ifdef SCHEDULER_MLFQ
  // Initialize the lock protecting the MLFQ enqueue sequence counter.
  initlock(&mlfq_seq_lock, "mlfq_seq");
#endif
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

#ifdef SCHEDULER_MLFQ
  // Every new process starts in Q0 with a fresh time slice!
  p->queue = 0;
  p->slice_ticks = 0;
  p->ticks_since_boost = 0;
  p->enqueue_seq = 0;
#endif

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
#ifdef SCHEDULER_MLFQ
  // Clear all MLFQ bookkeeping when the process is released!
  p->queue = 0;
  p->slice_ticks = 0;
  p->ticks_since_boost = 0;
  p->enqueue_seq = 0;
#endif
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");
  
  p->state = RUNNABLE;

#ifdef SCHEDULER_MLFQ
  // The initial process enters Q0 at the tail of that queue.
  mlfq_enqueue(p);
#endif

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

    acquire(&np->lock);
  np->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
  // A newly created process enters the tail of Q0.
  mlfq_enqueue(np);
#endif
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent == p) {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // Found one.
          pid = pp->pid;
          if (addr != 0 &&
              copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          pp->parent = 0;
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

#ifdef SCHEDULER_MLFQ
// Place a process at the tail of its current MLFQ queue.
//
// The caller must already hold p->lock. The sequence counter itself
// is protected because multiple CPUs may enqueue processes concurrently.
static void
mlfq_enqueue(struct proc *p)
{
  acquire(&mlfq_seq_lock);
  p->enqueue_seq = ++mlfq_next_seq;
  release(&mlfq_seq_lock);

  // Re-entering the queue gives the process a fresh time slice.
  p->slice_ticks = 0;
}
#endif

#ifdef SCHEDULER_MLFQ
// Move every existing process to Q0 after a 48-tick boost.
//
// The caller must ensure that a boost is pending.
static void
mlfq_priority_boost(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);

    if (p->state != UNUSED) {
      // Every process returns to the highest-priority queue.
      p->queue = 0;

      // Everyone receives a fresh time slice after the boost.
      p->slice_ticks = 0;

      // The boost resets this bookkeeping counter.
      p->ticks_since_boost = 0;

      // Reinsert processes in FIFO order at the boosted queue.
      acquire(&mlfq_seq_lock);
      p->enqueue_seq = ++mlfq_next_seq;
      release(&mlfq_seq_lock);
    }

    release(&p->lock);
  }
}
#endif

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
#ifdef SCHEDULER_MLFQ
  struct proc *chosen;
#endif
  struct cpu *c = mycpu();
  c->proc = 0;
  for (;;) {
    // Enable interrupts so this CPU can receive timer/device interrupts.
    intr_on();
    intr_off();
#ifdef SCHEDULER_MLFQ
  // Perform a pending 48-tick priority boost before selecting
  // the next process to run.
  if (mlfq_boost_pending) {
    mlfq_boost_pending = 0;
    mlfq_priority_boost();
  }
#endif
    int found = 0;
#ifdef SCHEDULER_MLFQ
    chosen = 0;
    // Find the highest-priority non-empty queue.
    // Within that queue, choose the process that has been waiting
    // the longest (smallest enqueue sequence number).
    for (int q = 0; q < 4 && chosen == 0; q++) {
      for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->state == RUNNABLE && p->queue == q) {
          if (chosen == 0 || p->enqueue_seq < chosen->enqueue_seq) {
            if (chosen != 0)
              release(&chosen->lock);

            chosen = p;
            continue;
          }
        }
        release(&p->lock);
      }
    }
    if (chosen != 0) {
      // Run the selected process.
      chosen->state = RUNNING;
      c->proc = chosen;
      swtch(&c->context, &chosen->context);
      c->proc = 0;
      // The process should have changed its state before returning here.
      release(&chosen->lock);
      found = 1;
    }
#else
    // Original xv6 round-robin scheduler.
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
#endif
  if (found == 0)
      asm volatile("wfi");
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

#ifdef SCHEDULER_MLFQ
// Check whether a runnable process exists in a higher-priority
// queue than the current process.
//
// This function is called from the timer-tick path while the
// current process may hold its own lock. Therefore, do not acquire
// any process locks here; doing so could create a lock-order
// deadlock with the scheduler.
static int
mlfq_higher_priority_runnable(int queue)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == RUNNABLE && p->queue < queue)
      return 1;
  }

  return 0;
}
#endif

#ifdef SCHEDULER_MLFQ
// Account for one timer tick of CPU time for the currently running process. If the process has exhausted its queue's time slice, demote it to the next lower-priority queue and request a yield!
int
mlfq_tick(void)
{
  struct proc *p = myproc();
  int slice_expired = 0;

  if (p == 0)
    return 0;

  acquire(&p->lock);

  // One timer tick of CPU time has been consumed by this process!
  p->slice_ticks++;

  // Queue 0, 1, 2, and 3 have time slices of 1, 4, 8, and 16 ticks respectively!
  int slice_limit;
  if (p->queue == 0)
    slice_limit = 1;
  else if (p->queue == 1)
    slice_limit = 4;
  else if (p->queue == 2)
    slice_limit = 8;
  else
    slice_limit = 16;

    // The current time slice has been completely consumed!
  if (p->slice_ticks >= slice_limit) {
    // Move the process down one priority level!
    // Q3 is already the lowest queue, so it remains in Q3!
    if (p->queue < 3)
      p->queue++;

    // A new time slice starts when the process is scheduled again!
    p->slice_ticks = 0;

    slice_expired = 1;
  }
  else if (mlfq_higher_priority_runnable(p->queue)) {
    // A higher-priority process is runnable, so give it the CPU
    // at the next scheduling point without changing our queue.
    slice_expired = 1;
  }

  release(&p->lock);

  return slice_expired;
}
#endif

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;

#ifdef SCHEDULER_MLFQ
  // Voluntary yield keeps the process in the same queue but moves it
  // to the tail of that queue with a fresh time slice.
  mlfq_enqueue(p);
#endif

  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (__atomic_load_n(&first, __ATOMIC_ACQUIRE)) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    // ensure other cores see first=0.
    __atomic_store_n(&first, 0, __ATOMIC_RELEASE);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;

      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNING.
        if (p->state == SLEEPING) {
          p->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
  // A waking process re-enters the tail of its current queue.
  mlfq_enqueue(p);
#endif
        }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
  // A waking process re-enters the tail of its current queue.
  mlfq_enqueue(p);
#endif
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    "unused",
    [USED]      "used",
    [SLEEPING]  "sleep ",
    [RUNNABLE]  "runble",
    [RUNNING]   "run   ",
    [ZOMBIE]    "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");

#ifdef SCHEDULER_MLFQ
  // Show MLFQ bookkeeping when the MLFQ scheduler is enabled.
  printk("PID  STATE   NAME             Q  SLICE  BOOST\n");
#else
  // Preserve the original xv6 process listing.
  printk("PID  STATE   NAME\n");
#endif

  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;

    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

#ifdef SCHEDULER_MLFQ
    printk("%d %s %s", p->pid, state, p->name);
    printk(" %d %d %d", p->queue, p->slice_ticks,
           p->ticks_since_boost);
    printk("\n");
#else
    printk("%d %s %s", p->pid, state, p->name);
    printk("\n");
#endif
  }
}
