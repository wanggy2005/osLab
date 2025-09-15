#include "kernel/types.h"
#include "user/user.h"

int main() {
    int c2f[2], f2c[2];
    pipe(c2f);
    pipe(f2c);
    char buf[16];

    int pid = fork();
    if (pid == 0) {
        // 子进程
        close(f2c[1]); // 关闭父->子管道的写端
        close(c2f[0]); // 关闭子->父管道的读端

        // 读取父进程发送的PID
        read(f2c[0], buf, sizeof(buf));
        int ppid = atoi(buf); 
        printf("%d: received ping from pid %d\n", getpid(), ppid);

        // 发送自己的PID给父进程
        itoa(getpid(), buf); 
        write(c2f[1], buf, strlen(buf) + 1); 

        close(f2c[0]);
        close(c2f[1]);
        exit(0);

    } else {
        // 父进程
        close(f2c[0]); // 关闭父->子管道的读端
        close(c2f[1]); // 关闭子->父管道的写端

        // 发送自己的PID给子进程
        itoa(getpid(), buf);
        write(f2c[1], buf, strlen(buf) + 1);

        // 读取子进程发送的PID
        read(c2f[0], buf, sizeof(buf));
        int cpid = atoi(buf);
        printf("%d: received pong from pid %d\n", getpid(), cpid);

        close(f2c[1]);
        close(c2f[0]);
        exit(0);
    }
}