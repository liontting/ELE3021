#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->mem_limit = 0;
  p->stack_size = 0;
  p->tid = 0;
  p->called = p;
  p->stack_start = 0;
  p->retval = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();
  struct proc *p;

  sz = curproc->sz;
  if(curproc->mem_limit != 0 && sz + n > curproc->mem_limit) // 추가적으로 할당 받는 memory가 limit보다 크다면
    return -1;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == curproc->pid) // 현재 pid와 같은 pid를 가졌다면
      p->sz = sz;              // sz를 갱신
  }
  release(&ptable.lock);

  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == curproc->pid && p != curproc){ // pid가 같고 curproc이 아니라면
      kfree(p->kstack);
      p->kstack = 0;
      p->pid = 0;
      p->parent = 0;
      p->name[0] = 0;
      p->killed = 0;
      p->state = UNUSED;
    } // 자원 할당 해제 부분, wait()에서 해제한 것과 동일 (page table은 제외)
  }
  release(&ptable.lock);

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid && p->tid == 0){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// 특정 프로세스에 대해 할당받을 수 있는 메모리의 최대치를 제한하는 함수
int
setmemorylimit(int pid, int limit)
{
  if (limit < 0) // limit의 값이 0보다 작으면
    return -1;
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){ // ptable 처음부터 끝까지 순회
    if(p->pid == pid){ // pid가 같으면
      if(limit == 0){ // limit가 0이면
        p->mem_limit = 0; // mem_limit에 0을 넣음
        release(&ptable.lock);
        return 0;
      }
      if(limit >= p->sz){ // 기존에 할당 받은 메모리보다 limit가 크면
        p->mem_limit = limit; // mem_limit에 limit를 넣음
        release(&ptable.lock);
        return 0;
      }
    }
  }
  release(&ptable.lock);
  return -1;
}

// setmemorylimit 함수의 system call 함수
int
sys_setmemorylimit(void)
{
  int pid, limit;
  if(argint(0, &pid) < 0 || argint(1, &limit) < 0) // pid, limit 인자가 int로 들어오지 않았다면
    return -1;
  return setmemorylimit(pid, limit); // 인자를 넣어 setmemorylimit 함수 실행
}

// 현재 실행 중인 프로세스들의 정보를 출력하는 함수
void
printlist()
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){ // ptable 처음부터 끝까지 순회
    if(p->state == RUNNABLE || p->state == RUNNING || p->state == SLEEPING){ // 현재 실행 중인 process라면
      cprintf("name: %s, pid: %d, pages for stack: %d\n", p->name, p->pid, p->stack_size);
      if (p->mem_limit == 0)                          // mem_limit이 0이면
        cprintf("memory size: %d, memory limit: unlimited\n", p->sz);
      else                                            // mem_limit이 0이 아니면
        cprintf("memory size: %d, memory limit: %d\n", p->sz, p->mem_limit);
    }
  }
  release(&ptable.lock);
}

// printlist 함수의 system call 함수
int
sys_printlist(void)
{
  printlist();
  return 0;
}

// 새 스레드를 생성하고 시작하는 함수
int
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{
  int i;
  struct proc *np;
  struct proc *p;
  struct proc *curproc = myproc();
  uint sz, sp, ustack[2];

  // fork에서 변형
  if((np = allocproc()) == 0){ // 새로운 thread를 위한 공간을 np에 할당
    return -1;                 // 종료
  }

  if(curproc->pgdir == 0){ // 현재 curproc의 page directory가 0이면
    np->state = UNUSED;    // 상태를 UNUSED로 바꾸고
    return -1;             // 종료
  }
  np->parent = curproc->parent; // 새로운 thread의 parent를 현재 curproc의 parent로 설정
  *np->tf = *curproc->tf;       // 새로운 thread의 trap frame을 현재 curproc의 trap frame으로 설정

  np->tf->eax = 0;  // Clear %eax so that fork returns 0 in the child.

  for(i = 0; i < NOFILE; i++)                    // 0부터 file table의 최대 크기까지
    if(curproc->ofile[i])                        // file table에서 해당 file이 비어있지 않으면
      np->ofile[i] = filedup(curproc->ofile[i]); // 새로운 thread의 file을 현재 curproc의 file으로 설정
  np->cwd = idup(curproc->cwd);                  // 새로운 thread의 cwd을 현재 curproc의 cwd으로 설정

  safestrcpy(np->name, curproc->name, sizeof(curproc->name)); // 새로운 thread의 name을 현재 curproc의 name으로 설정

  acquire(&ptable.lock);
  
  np->pid = curproc->pid; // np의 pid를 현재 curproc의 pid로 설정

  np->tid = nexttid++;    // np의 tid를 설정

  np->called = curproc;   // thread_create를 호출한 curproc의 정보 저장

  release(&ptable.lock);

  // stack 수정 부분 (exec에서 살짝 변형)
  sz = curproc->sz; // sz에 현재 curproc의 sz를 할당

  if((sz = allocuvm(curproc->pgdir, sz, sz + 2*PGSIZE)) == 0) // 2만큼의 가상 메모리 공간을 할당
    goto bad;
  clearpteu(curproc->pgdir, (char*)(sz - 2*PGSIZE));          // 가드용 페이지를 설정
  sp = sz; // stack pointer에 sz를 할당

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = (uint)arg;   // 받은 인자를 저장

  sp -= 2*4; // sp를 2칸 감소
  if(copyout(curproc->pgdir, sp, ustack, 2*4) < 0) // ustack의 data를 pgdir에 복사함
    goto bad;

  np->stack_start = sz - 2*PGSIZE; // np의 stack의 시작 위치를 저장
  curproc->sz = sz;                // 현재 curproc의 sz에 바뀐 sz 값을 할당
  
  np->sz = sz;                       // np의 sz에 sz 값을 할당
  np->pgdir = curproc->pgdir;        // np의 pgdir에 현재 curproc의 pgdir를 할당
  np->tf->eip = (uint)start_routine; // instruction pointer에 start_routine를 저장
  np->tf->esp = sp;                  // stack pointer에 sp를 담음

  *thread = np->tid;                 // thread에 np의 tid를 넣음

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->pid == curproc->pid) // 현재 pid와 같은 pid를 가졌다면
      p->sz = sz;              // sz를 갱신
  
	np->state = RUNNABLE;              // np의 상태를 RUNNABLE로 설정

	release(&ptable.lock);

  return 0;

 bad:
  np->state = UNUSED;                // np의 상태를 UNUSED로 설정
  return -1;
}

// thread_create 함수의 system call 함수
int
sys_thread_create(void)
{
  int thread, start_routine, arg;

  if(argint(0, &thread) < 0 || argint(1, &start_routine) < 0 || argint(2, &arg) < 0)
    return -1;
  return thread_create((thread_t *)thread, (void *)start_routine, (void *)arg);
}

// 스레드를 종료하고 값을 반환하는 함수
void
thread_exit(void *retval)
{
  struct proc *curproc = myproc();
  int fd;

  if(curproc == initproc)   // curproc가 initproc인 경우
    panic("init exiting");

  curproc->retval = retval; // retval값을 지정해줌

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){ // 0부터 NOFILE까지
    if(curproc->ofile[fd]){ // 열린 file이 있다면
      fileclose(curproc->ofile[fd]); // 열린 file을 닫아줌
      curproc->ofile[fd] = 0; // 참조한 값 초기화
    }
  }

  begin_op(); // file system 동기화를 위해 호출
  iput(curproc->cwd); // 현재 cwd를 file system에 반환
  end_op(); // file system 동기화를 종료
  curproc->cwd = 0; // 참조한 값 초기화

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->called); // exit()하려는 curproc을 호출한 called를 wakeup

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE; // 상태를 ZOMBIE로 설정
  sched();
  panic("zombie exit");
}

// thread_exit 함수의 system call 함수
int
sys_thread_exit(void)
{
  int retval;

  if(argint(0, &retval) < 0)
    return -1;
  thread_exit((void *)retval);
  return 0;
}

// 해당 스레드의 종료를 기다리고, 스레드가 thread_exit을 통해 반환한 값을 반환하는 함수
int
thread_join(thread_t thread, void **retval)
{
  struct proc *p;
  int havekids;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){ // ptable 처음부터 끝까지 순회
      if(p->called != curproc || p->tid != thread) // p을 호출한 called가 curproc이 아니거나, tid가 thread가 아닌 경우
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){ // 상태가 ZOMBIE인 경우
        // Found one.
        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        // 기존의 각종 초기화 + 새로 만든 값 초기화
        p->called = 0;
        p->tid = 0;
        p->stack_start = 0;
        *retval = p->retval; // retval에 p에 넣어놨던 retval 값을 할당
        release(&ptable.lock);
        return 0;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// thread_join 함수의 system call 함수
int
sys_thread_join(void)
{
  int thread, retval;

  if(argint(0, &thread) < 0 || argint(1, &retval) < 0)
    return -1;
  return thread_join((thread_t)thread, (void **)retval);
}

// exec에서 pid가 같으면서 tid가 다른 thread 정리하는 함수
void
exec_exit(int pid, int tid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == pid && p->tid != tid) {
      kfree(p->kstack);
      p->kstack = 0;
      p->pid = 0;
      p->parent = 0;
      p->name[0] = 0;
      p->killed = 0;
      p->state = UNUSED;
    }
  }
  release(&ptable.lock);
}