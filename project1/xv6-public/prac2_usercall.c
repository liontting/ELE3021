#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
    __asm__("int $128");
    return 0;
};