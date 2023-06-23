#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  if(argc != 4){ // 옵션이 추가되었기 때문에 4개 인자인지 체크
    printf(2, "Usage: ln [-h or -s] [old] [new]\n");
    exit();
  }
  if(!strcmp(argv[1], "-h")){ // 기존의 hard link라면
    if(link(argv[2], argv[3]) < 0)
      printf(2, "hard link %s %s: failed\n", argv[2], argv[3]);
    exit();
  }
  else if(!strcmp(argv[1], "-s")){ // symbolic link라면
    if(symbolic_link(argv[2], argv[3]) < 0) // symbolic link 실행
      printf(2, "symbolic link %s %s: failed\n", argv[2], argv[3]);
    exit();
  }
}
