# HYU_ELE3021
### 2023년 1학기 Operating System (Prof. Sooyong Kang)

## Project 1
* MLFQ Scheduler Implementation
  * 3-level feedback queue

100/100점

## Project 2
### Process Management
* Process with various stack size
  * int exec2(char *path, char **argv, int stacksize);
* Process memory limitation
  * int setmemorylimit(int pid, int limit);
* Process manager
  * List
  * Kill
  * Execute
  * Memlim
  * Exit
### LWP (Light-weight process)
* Pthread in xv6
  * int thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg);
  * int thread_join(thread_t thread, void **retval);
  * void thread_exit(void *retval);
  * Other system calls(fork, exec, wait…)

100/100점

## Project 3
### File system
* Multi Indirect
* Symbolic Link
* Sync
  * (해당 부분의 구현이 미흡해 약간의 감점)

80/100점
