#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  SETGATE(idt[128], 1, SEG_KCODE<<3, vectors[128], DPL_USER);
  SETGATE(idt[129], 1, SEG_KCODE<<3, vectors[129], DPL_USER);
  SETGATE(idt[130], 1, SEG_KCODE<<3, vectors[130], DPL_USER);
  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  if(tf->trapno == 128){
    mycall();
    exit();
  }
  if(tf->trapno == 129){
    schedulerLock(2019042497);
  }
  if(tf->trapno == 130){
    schedulerUnlock(2019042497);
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER){
    global_ticks++;                                                     // global_tick 1 증가
    myproc()->t_quantum++;                                              // 해당 process의 time quantum 1 증가

    if(myproc()->t_quantum == 2*(myproc()->q_level)+4){                 // 만약 해당 process의 time quantum이 2*n + 4일 경우
      if(myproc()->q_level == 2){                                       // L2 queue에 있는 process라면
        if(myproc()->priority != 0){                                    // priority가 0이 아니라면
          setPriority(myproc()->pid, (myproc()->priority)-1);           // priority를 1 감소
        }
        myproc()->t_quantum = 0;                                        // 해당 process의 time quantum을 0으로 초기화
      }
      else{                                                             // 만약 L0나 L1 queue에 있는 process라면
        myproc()->q_level++;                                            // queue level을 1 증가
        myproc()->t_quantum = 0;                                        // 해당 process의 time quantum을 0으로 초기화
        myproc()->order = MLFQ_order[myproc()->q_level]++;              // 해당 process의 순서를 해당 queue의 마지막으로 보냄
      }
    }
    yield();                                                            // 다음 process에게 CPU를 양보
  }

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
