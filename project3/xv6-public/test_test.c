#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"

#define FILESIZE        (16*1024*1024)  // 16 MB
#define BUFSIZE         512
#define BUF_PER_FILE    ((FILESIZE) / (BUFSIZE))

int
main(int argc, char *argv[])
{
    int fd, i, j; 
    int r;
    char *path = (argc > 1) ? argv[1] : "hugefile";
    char data[BUFSIZE];
    char buf[BUFSIZE];

    printf(1, "huge file test starting\n");
    const int sz = sizeof(data);
    for (i = 0; i < sz; i++) {
        data[i] = i % 128;
    }

    printf(1, "1. create test\n");
    fd = open(path, O_CREATE | O_RDWR);
    for(i = 0; i < BUF_PER_FILE; i++){
        if (i % 100 == 0){
            printf(1, "%d bytes written\n", i * BUFSIZE);
        }
        if ((r = write(fd, data, sizeof(data))) != sizeof(data)){
            printf(1, "write returned %d : failed\n", r);
            exit();
        }
    }
    printf(1, "%d bytes written\n", BUF_PER_FILE * BUFSIZE);
    close(fd);
    printf(1, "1. create test finished\n");

    printf(1, "2. read test\n");
    fd = open(path, O_RDONLY);
    for (i = 0; i < BUF_PER_FILE; i++){
        if (i % 100 == 0){
            printf(1, "%d bytes read\n", i * BUFSIZE);
        }
        if ((r = read(fd, buf, sizeof(data))) != sizeof(data)){
            printf(1, "read returned %d : failed\n", r);
            exit();
        }
        for (j = 0; j < sz; j++) {
            if (buf[j] != data[j]) {
                printf(1, "data inconsistency detected\n");
                exit();
            }
        }
    }
    printf(1, "%d bytes read\n", BUF_PER_FILE * BUFSIZE);
    close(fd);
    printf(1, "2. read test finished\n");

    printf(1, "3. stress test\n");
    if(unlink(path) < 0){
        printf(1, "rm: %s failed to delete\n", path);
        exit();
    }

    fd = open(path, O_CREATE | O_RDWR);
    for(i = 0; i < BUF_PER_FILE; i++){
        if (i % 100 == 0){
            printf(1, "%d bytes written\n", i * BUFSIZE);
        }
        if ((r = write(fd, data, sizeof(data))) != sizeof(data)){
            printf(1, "write returned %d : failed\n", r);
            exit();
        }
    }
    printf(1, "%d bytes written\n", BUF_PER_FILE * BUFSIZE);
    close(fd);
    printf(1, "3. stress test finished\n");

    exit();
}