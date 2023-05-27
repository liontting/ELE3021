#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int
getcmd(char *buf, int nbuf)
{
  printf(2, "pmanager$ ");
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;
  return 0;
}

int
main(void)
{
  static char buf[100];
  char temp[100];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    buf[strlen(buf)-1] = 0;  // chop \n
    int i;
    for(i = 0; i < 100; i++){
      if (buf[i] == ' ' || buf[i] == 0){ // 공백이나 0(NULL)을 만나면
        temp[i] = 0;                     // 0으로 temp의 마지막을 닫아줌
        break;
      }
      temp[i] = buf[i];                  // temp에 buf 값 복사
    }
    i++;                                 // 한 칸 뒤로 이동
    int n = i;                           // 현재 위치 기억
    if(!strcmp(temp, "list")){ // 만약 list 명령이라면
      printlist();             // 정보 출력
    }
    else if(!strcmp(temp, "kill")){ // 만약 kill 명령이라면
      char bufpid[20];
      int pid;
      for(; i < 100; i++){
        if (buf[i] == 0){           // 0(NULL)을 만나면
          bufpid[i - n] = 0;        // 0으로 bufpid의 마지막을 닫아줌
          break;
        }
        bufpid[i - n] = buf[i];     // bufpid에 buf 값 복사
      }
      pid = atoi(bufpid);           // bufpid에 있는 값을 int로 변환해 pid에 담음
      if(!kill(pid)){               // kill(pid)를 실행했을 때 0이 반환되면
        printf(2, "kill %d successed\n", pid);
      }
      else{
        printf(2, "kill %d failed\n", pid);
      }
    }
    else if(!strcmp(temp, "execute")){            // 만약 execute 명령이라면
      char* argv[10] = { 0, };
      char bufpath[70];
      char bufsize[20];
      int stacksize;
      for(; i < 100; i++){
        if (buf[i] == ' '){                       // 공백을 만나면
          bufpath[i - n] = 0;                     // 0으로 bufpath의 마지막을 닫아줌
          break;
        }
        bufpath[i - n] = buf[i];                  // bufpath에 buf 값 복사
      }
      strcpy(argv[0], bufpath);                   // argv[0]에 bufpath 값 복사
      i++;                                        // 한 칸 뒤로 이동
      n = i;                                      // 현재 위치 기억
      for(; i < 100; i++){
        if (buf[i] == 0){                         // 0(NULL)을 만나면
          bufsize[i - n] = 0;                     // 0으로 bufsize의 마지막을 닫아줌
          break;
        }
        bufsize[i - n] = buf[i];                  // bufsize에 buf 값 복사
      }
      stacksize = atoi(bufsize);                  // bufsize에 있는 값을 int로 변환해 stacksize에 담음
      if(fork() == 0){                            // fork() 했을 때 자식 process이면
        if(exec2(bufpath, argv, stacksize) == -1) // 인자를 알맞게 넣고 exec2() 실행했을 때 -1이 반환되면
            printf(2, "execute failed\n");
        break;
      }
    }
    else if(!strcmp(temp, "memlim")){  // 만약 memlim 명령이라면
      char bufpid[20], buflim[20];
      int pid, limit;
      for(; i < 100; i++){
        if (buf[i] == ' '){            // 공백을 만나면
          bufpid[i - n] = 0;           // 0으로 bufpid의 마지막을 닫아줌
          break;
        }
        bufpid[i - n] = buf[i];        // bufpid에 buf 값 복사
      }
      pid = atoi(bufpid);              // bufpid에 있는 값을 int로 변환해 pid에 담음
      i++;                             // 한 칸 뒤로 이동
      n = i;                           // 현재 위치 기억
      for(; i < 100; i++){
        if (buf[i] == 0){              // 0(NULL)을 만나면
          buflim[i - n] = 0;           // 0으로 buflim의 마지막을 닫아줌
          break;
        }
        buflim[i - n] = buf[i];        // buflim에 buf 값 복사
      }
      limit = atoi(buflim);            // buflim에 있는 값을 int로 변환해 lim에 담음
      if(!setmemorylimit(pid, limit)){ // 인자를 알맞게 넣고 setmemorylimit() 실행했을 때 0이 반환되면
        printf(2, "memlim %d: %d successed\n", pid, limit);
      }
      else{
        printf(2, "memlim %d: %d failed\n", pid, limit);
      }
    }
    else if(!strcmp(temp, "exit")){ // 만약 exit 명령이라면
      exit();                       // exit() 호출
    }
  }
  exit();
}