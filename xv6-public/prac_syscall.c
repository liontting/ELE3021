#include "types.h"
#include "defs.h"

int myfunction(char *str) {
	cprintf("%s\n", str);
	return 0xABCDABCD;
}

int sys_myfunction(void) {
	char *str;

	if(argstr(0, &str) < 0)
		return -1;
	return myfunction(str);
}
