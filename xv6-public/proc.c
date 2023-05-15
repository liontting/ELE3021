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

uint global_ticks = 0;
uint MLFQ_order[3] = {1,1,1};
int nextpid = 1;
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
  p->q_level = 0;
  p->t_quantum = 0;
  p->priority = 3;
  p->order = MLFQ_order[0]++;
  p->boosting_tmp = 0;
  p->qualification = 0;

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

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
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
    struct proc *new_p = 0;                             // 다음 실행될 process를 담는 변수
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){ // ptable의 process 처음부터 끝까지 탐색
      if(p->state != RUNNABLE)                          // RUNNABLE 하지 않다면
        continue;                                       // 다음 process로 넘어감
      if(p->qualification){                             // 만약 qualification이 1인 process가 있다면
        new_p = p;                                      // 실행될 process를 담는 변수에 넣어줌
        break;                                          // lock이기 때문에 무조건 실행되어야 하므로 break
      }
      if(!new_p || new_p->q_level > p->q_level)         // 만약 기존에 아무것도 없었거나, 기존에 있던 process보다 queue level이 낮은 경우
        new_p = p;                                      // 실행될 process를 담는 변수에 넣어줌
      else if(new_p->q_level == p->q_level){            // 기존의 process와 queue level이 같은 경우
        if(p->q_level == 0 || p->q_level == 1)          // L0이나 L1에 있는 process인 경우
          if(new_p->order > p->order)                   // 현재 queue 안의 order가 더 낮다면
            new_p = p;                                  // 실행될 process를 담는 변수에 넣어줌
        if(p->q_level == 2){                            // L2에 있는 process인 경우
          if(new_p->priority > p->priority)             // priority 값이 더 낮으면
            new_p = p;                                  // 실행될 process를 담는 변수에 넣어줌
          else if(new_p->priority == p->priority){      // priority 값이 같다면
            if(new_p->order > p->order)                 // 현재 queue 안의 order가 더 낮다면
              new_p = p;                                // 실행될 process를 담는 변수에 넣어줌
          }
        }
      }
    }
    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    if(new_p){
      c->proc = new_p;
      switchuvm(new_p);
      new_p->state = RUNNING;

      swtch(&(c->scheduler), new_p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    if(global_ticks == 100){ // global_ticks이 100이 되었다면
      priority_boosting();   // Starvation을 막기 위해 priority boosting
    }
    release(&ptable.lock);

  }
}

// Priority boosting 함수
void
priority_boosting()
{
  struct proc *p;                                        // process를 찾는 for문을 돌리기 위해 필요한 process를 담는 변수
  uint now_l0_order = MLFQ_order[0] - 1;                 // 현재 L0이 사용한 order 값
  int check = 1;                                         // 모든 process가 boosting이 되었는지 check하는 변수
  while(check){                                          // check가 1인 경우 while문 반복
    struct proc *tmp = 0;                                // priority boosting 시 순서를 유지하기 위해 다음 순서를 담는 변수
    check = 0;                                           // check를 0으로 설정
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){  // ptable의 process 처음부터 끝까지 탐색
      if(p->state != RUNNABLE || p->boosting_tmp != 0)   // RUNNABLE 하지 않거나, 이미 boosting 된 대상이면
        continue;                                        // 다음 process로 넘어감
      check = 1;                                         // 그렇지 않다면 boosting 대상이 있으므로 check를 1로 바꿈
      if(p->qualification){                              // 만약 qualification이 1이라면
        p->qualification = 0;                            // lock을 해제함
        tmp = p;                                         // 다음 boosting의 대상으로 넣음
        break;                                           // 가장 먼저 L0에 들어가야 하기 때문에 for문 탈출
      }
      if (!tmp || tmp->q_level > p->q_level)             // 만약 기존에 아무것도 없었거나, 기존에 있던 process보다 queue level이 낮은 경우
        tmp = p;                                         // 다음 boosting의 대상으로 넣음
      else if (tmp->q_level == p->q_level) {             // 기존의 process와 queue level이 같은 경우
        if (p->q_level == 0 || p->q_level == 1)          // L0이나 L1에 있는 process인 경우
          if (tmp->order > p->order)                     // 현재 queue 안의 order가 더 낮다면
            tmp = p;                                     // 다음 boosting의 대상으로 넣음
        if (p->q_level == 2) {                           // L2에 있는 process인 경우
          if (tmp->priority > p->priority)               // priority 값이 더 낮으면
            tmp = p;                                     // 다음 boosting의 대상으로 넣음
          else if (tmp->priority == p->priority) {       // priority 값이 같다면
            if (tmp->order > p->order)                   // 현재 queue 안의 order가 더 낮다면
              tmp = p;                                   // 다음 boosting의 대상으로 넣음
          }
        }
      }
    }
    if(check == 0)                                       // 더이상 boosting의 대상이 남아있지 않으면(check가 0)
      break;                                             // while문 탈출
    tmp->boosting_tmp = MLFQ_order[0]++;                 // boosting의 대상을 L0의 다음 순서로 넣음
  }
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){    // ptable의 process 처음부터 끝까지 탐색
    if(p->boosting_tmp == 0)                             // boosting의 대상이 아니면
      continue;                                          // 다음 process로 넘어감
    p->q_level = 0;                                      // queue level 0으로 초기화
    p->priority = 3;                                     // priority 3으로 초기화
    p->t_quantum = 0;                                    // time quantum 0으로 초기화
    p->order = p->boosting_tmp - now_l0_order;           // order에 1부터 넣음
    p->boosting_tmp = 0;                                 // 임시로 순서 담았던 변수 0으로 초기화
  }
  MLFQ_order[0] = MLFQ_order[0] - now_l0_order;          // 현재 L0에 있는 개수로 표시
  MLFQ_order[1] = 1;                                     // L1에는 아무 process도 없으므로 1로 초기화
  MLFQ_order[2] = 1;                                     // L2에는 아무 process도 없으므로 1로 초기화
  global_ticks = 0;                                      // global_ticks 0으로 초기화
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
  if (myproc()->q_level != 2)                           // L0과 L1에 있는 process일 경우
    myproc()->order = MLFQ_order[myproc()->q_level]++;  // 그 queue의 마지막 순서로 넣음
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// yield 함수의 system call 함수
int
sys_yield(void)
{
  yield();  // yield를 실행
  return 0; // 0을 return
}

// process가 속한 queue의 level을 반환하는 함수
int
getLevel(void)
{
  int level = myproc()->q_level;  // 해당 process의 queue level을 level에 담음
  return level;                   // queue level 반환
}

// getLevel 함수의 system call 함수
int
sys_getLevel(void)
{
  return getLevel(); // getLevel()에서 return 된 queue level 값을 return
}

// 해당 pid의 process의 priority를 설정하는 함수
void
setPriority(int pid, int priority)
{
  struct proc *p;                                             // process를 찾는 for문을 돌리기 위해 필요한 process를 담는 변수
  int check = 0;                                              // priority가 설정되었는지 check하는 역할을 하는 변수

  if(priority < 0 || priority > 3)                            // priority의 값이 0~3 사이가 아닌 경우
    cprintf("setPriority error\n");                           // error 문구 출력
  else{                                                       // priority의 값이 0~3 사이인 경우
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);                                    // ptable에 접근해 값을 수정해야 하기 때문에 lock을 얻음
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){       // ptable의 process 처음부터 끝까지 탐색
      if(p->pid == pid){                                      // 해당 pid를 가진 process를 찾으면
        p->priority = priority;                               // 해당 process에 priority 값을 넣음
        check = 1;                                            // priority가 설정되었으므로 check를 1로
        break;                                                // riority가 설정되었으면 반복문 탈출
      }
    }
    release(&ptable.lock);                                    // ptable lock을 해제
    if(check == 0)                                            // priority를 설정할 pid가 없었을 경우
      cprintf("setPriority error\n");                         // error 문구 출력
  }
}

// setPriority 함수의 system call 함수
int
sys_setPriority(void)
{
  int pid;                        // setPriority 함수가 받는 pid 인자
  int priority;                   // setPriority 함수가 받는 priority 인자

  if(argint(0, &pid) < 0)         // pid 인자가 int로 들어오지 않았다면
    return -1;                    // -1 return해 오류임을 표시
  if(argint(1, &priority) < 0)    // priority 인자가 int로 들어오지 않았다면
    return -1;                    // -1 return해 오류임을 표시
  setPriority(pid, priority);     // 인자를 알맞게 넣어 setPriority를 실행
  return 0;                       // 0을 return
}

// 해당 프로세스가 우선적으로 스케줄링 되도록 하는 함수
void
schedulerLock(int password)
{
  int check = 0;                                                                   // scheduler lock이 걸린 process가 있는지 확인하는 변수
  if(password == 2019042497){                                                      // 암호가 학번과 일치할 시
    struct proc *p;                                                                // process를 찾는 for문을 돌리기 위해 필요한 process를 담는 변수
    acquire(&ptable.lock);                                                         // ptable lock
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){                            // ptable의 process 처음부터 끝까지
      if(p->qualification == 1 && (p->state == RUNNABLE || p->state == RUNNING)){  // scheduler lock 상태인 process가 있다면
        check = 1;                                                                 // check를 1로
        break;                                                                     // 찾아서 넣었으면 반복문 탈출
      }
    }
    release(&ptable.lock);                                                         // ptable lock 해제
    if (check == 0){                                                               // scheduler lock이 걸린 proecss가 없다면 (check가 0이라면)
      myproc()->qualification = 1;                                                 // 우선으로 처리되어야 할 자격을 얻음
      global_ticks = 0;                                                            // global tick은 priority boosting 없이 0으로 초기화
    }
  }
  if(password != 2019042497 || check == 1){                                        // 암호가 일치하지 않거나 이미 scheduler lock이 걸린 process가 있는 경우
    cprintf("pid: %d, time quantum: %d, current queue level: %d\n",
            myproc()->pid, myproc()->t_quantum, myproc()->q_level);                // 프로세스의 pid, time quantum, 현재 위치한 큐의 level을 출력
    exit();                                                                        // 종료
  }
}

// schedulerLock 함수의 system call 함수
int
sys_schedulerLock(void)
{
  int password;                 // schedulerLock 함수가 받는 password 인자

  if(argint(0, &password) < 0)  // password 인자가 int로 들어오지 않았다면
    return -1;                  // -1 return해 오류임을 표시
  schedulerLock(password);      // 인자를 알맞게 넣어 schedulerLock 함수 실행
  return 0;                     // 0을 return
}

// 해당 프로세스가 우선적으로 스케줄링 되던 것을 중지하는 함수
void
schedulerUnlock(int password)
{
  if(password == 2019042497 && myproc()->qualification == 1){ // 암호가 일치하고, 자격이 있을 시
    myproc()->qualification = 0;                              // 우선으로 처리되어야 할 자격을 해제
    myproc()->q_level = 0;                                    // L0 queue로 이동
    myproc()->priority = 3;                                   // priority를 3으로 설정
    myproc()->t_quantum = 0;                                  // time quantum 초기화
    myproc()->order = 0;                                      // L0 queue의 가장 앞 순서로 지정
  }
  else{                                                       // 암호가 일치하지 않거나 자격이 없을 시
    cprintf("pid: %d, time quantum: %d, current queue level: %d\n",
            myproc()->pid, myproc()->t_quantum, myproc()->q_level);
    // 프로세스의 pid, time quantum, 현재 위치한 큐의 level을 출력
    exit();                                                   // 종료
  }
}

// schedulerUnlock 함수의 system call 함수
int
sys_schedulerUnlock(void)
{
  int password;                 // schedulerUnlock 함수가 받는 password 인자

  if(argint(0, &password) < 0)  // password 인자가 int로 들어오지 않았다면
    return -1;                  // -1 return해 오류임을 표시
  schedulerUnlock(password);    // 인자를 알맞게 넣어 schedulerUnlock 함수 실행
  return 0;                     // 0을 return
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
    if(p->pid == pid){
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
