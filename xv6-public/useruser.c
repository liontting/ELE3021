#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_LOOP 100000
#define NUM_YIELD 20000
#define NUM_SLEEP 500

#define NUM_THREAD 4
#define MAX_LEVEL 3
#define PASSWORD 2019042497

int parent, child;

int fork_children()
{
  int i, p;
  for (i = 0; i < NUM_THREAD; i++)
    if ((p = fork()) == 0)
    {
      sleep(10);
      return getpid();
    }
  return parent;
}


int fork_children2()
{
  int i, p;
  for (i = 0; i < NUM_THREAD; i++)
  {
    if ((p = fork()) == 0)
    {
      sleep(10);
      return getpid();
    }
    else
    {
      setPriority(p, i);
      printf(1, "pid: %d, priority: %d\n", p, i);
    }
  }
  return parent;
}

void exit_children()
{
  if (getpid() != parent)
    exit();
  while (wait() != -1);
}

int main(int argc, char *argv[])
{
  int i, pid;
  int count[MAX_LEVEL] = {0};

  parent = getpid();

  printf(1, "MLFQ test start\n");

  printf(1, "[Test 1] default\n");
  pid = fork_children();

  if (pid != parent)
  {
    for (i = 0; i < NUM_LOOP; i++)
    {
      int x = getLevel();
      if (x < 0 || x > 2)
      {
        printf(1, "Wrong level: %d\n", x);
        exit();
      }
      count[x]++;
    }
    printf(1, "Process %d\n", pid);
    for (i = 0; i < MAX_LEVEL; i++)
      printf(1, "pid :%d, L%d: %d\n", pid, i, count[i]);
  }
  exit_children();
  printf(1, "[Test 1] finished\n");

  printf(1, "[Test 2] priority\n");
  pid = fork_children2();

  if (pid != parent)
  {
    for (i = 0; i < NUM_LOOP; i++)
    {
      int x = getLevel();
      if (x < 0 || x > 2)
      {
        printf(1, "Wrong level: %d\n", x);
        exit();
      }
      count[x]++;
    }
    printf(1, "Process %d\n", pid);
    for (i = 0; i < MAX_LEVEL; i++)
      printf(1, "pid :%d, L%d: %d\n", pid, i, count[i]);
  }
  exit_children();
  printf(1, "[Test 2] finished\n");
  
  printf(1, "[Test 3] 13 yield\n");
  pid = fork_children();

  if (pid != parent)
  {
    for (i = 0; i < NUM_LOOP; i++)
    {
      int x = getLevel();
      if (x < 0 || x > 2)
      {
        printf(1, "Wrong level: %d\n", x);
        exit();
      }
      count[x]++;
      if(getpid() == 13)
        yield();
    }
    printf(1, "Process %d\n", pid);
    for (i = 0; i < MAX_LEVEL; i++)
      printf(1, "pid :%d, L%d: %d\n", pid, i, count[i]);
  }
  exit_children();
  printf(1, "[Test 3] finished\n");
  
  printf(1, "[Test 4] setPriority\n");
  child = fork();

  if (child == 0)
  {
    int grandson;
    sleep(10);
    grandson = fork();
    if (grandson == 0)
    {
      setPriority(getpid() + 2, 0);
      setPriority(getpid() + 3, 0);
    }
    else
    {
      setPriority(grandson, 0);
      setPriority(getpid() + 1, 0);
    }
    sleep(20);
    wait();
  }
  else
  {
    int child2 = fork();
    sleep(20);
    if (child2 == 0)
      sleep(10);
    else
    {
      setPriority(child, -1);
      setPriority(child, 11);
      setPriority(parent, 5);
    }
  }
  exit_children();
  printf(1, "[Test 4] finished\n");

  printf(1, "[Test 5] schedulerLock / Unlock, Normal Case\n");
  child = fork();
  if (child == 0) { //child
    printf(1, "Process %d scheduler Lock\n", getpid());
    schedulerLock(PASSWORD);
    printf(1, "Process %d scheduler Lock success\n", getpid());
    printf(1, "Process %d scheduler Unlock\n", getpid());
    schedulerUnlock(PASSWORD);
    printf(1, "Process %d scheduler Unlock success\n", getpid());
  }
  else { // parent
    printf(1, "Process %d scheduler Lock\n", getpid());
    schedulerLock(PASSWORD);
    printf(1, "Process %d scheduler Lock success\n", getpid());
    printf(1, "Process %d scheduler Unlock\n", getpid());
    schedulerUnlock(PASSWORD);
    printf(1, "Process %d scheduler Unlock success\n", getpid());
  }
  exit_children();
  printf(1, "[Test 5] finished\n");

  printf(1, "[Test 6] schedulerLock / Unlock, Wrong Case1 : PASSWORD ERROR\n");
  child = fork();
  if (child == 0) { //child
    printf(1, "Process %d scheduler Lock, Wrong Password\n", getpid());
    schedulerLock(PASSWORD + 1);
    // 아래 부분 실행 안 되어야 함
    printf(1, "Process %d scheduler Lock success\n", getpid());
    schedulerUnlock(PASSWORD + 1);
    printf(1, "Process %d scheduler Unlock success\n", getpid());
  }
  else { // parent
    int child2 = fork();
    if (child2 == 0) {
      printf(1, "Process %d scheduler Lock\n", getpid());
      schedulerLock(PASSWORD);
      printf(1, "Process %d scheduler Lock success\n", getpid());
      printf(1, "Process %d scheduler Unlock, Wrong Password\n", getpid());
      schedulerUnlock(PASSWORD + 1);
      // 아래 부분 실행 안 되어야 함
      printf(1, "Process %d scheduler Unlock success\n", getpid());
    }
  }
  exit_children();
  printf(1, "[Test 6] finished\n");

  printf(1, "[Test 7] schedulerLock / Unlock Wrong Case2 : duplication ERROR\n");
  child = fork();
  if (child == 0) { //child
    printf(1, "Process %d scheduler Lock\n", getpid());
    schedulerLock(PASSWORD);
    printf(1, "Process %d scheduler Lock success\n", getpid());
    printf(1, "Process %d scheduler Lock, duplication\n", getpid());
    // 아래 부분 실행 안 되어야 함
    schedulerLock(PASSWORD);
    printf(1, "Process %d scheduler Lock success\n", getpid());
  }
  else { // parent
    int child2 = fork();
    if (child2 == 0) {
      printf(1, "Process %d scheduler Lock\n", getpid());
      schedulerLock(PASSWORD);
      printf(1, "Process %d scheduler Lock success\n", getpid());
      printf(1, "Process %d scheduler Unlock\n", getpid());
      schedulerUnlock(PASSWORD);
      printf(1, "Process %d scheduler Unlock success\n", getpid());
      printf(1, "Process %d scheduler Unlock, duplication\n", getpid());
      schedulerUnlock(PASSWORD);
      // 아래 부분 실행 안 되어야 함
      printf(1, "Process %d scheduler Unlock success\n", getpid());
    }
  }
  exit_children();
  printf(1, "[Test 7] finished\n");

  printf(1, "done\n");
  exit();
}