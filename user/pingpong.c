#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    int parent_fd[2],child_fd[2];

    pipe(parent_fd);
    pipe(child_fd);

    if(fork() == 0) {       //child process
        //close unused fd
        close(parent_fd[1]);
        close(child_fd[0]);
        char buf[512];
        //read from the pipe
        read(parent_fd[0],buf,512);
        close(parent_fd[0]);
        printf("%d: received ",getpid());
        printf("%s\n",buf);
        //write to the pipe
        write(child_fd[1],"pong\en",6);
        close(child_fd[1]);
        exit();
    } else {
        close(parent_fd[0]); 
        close(child_fd[1]);
        write(parent_fd[1], "ping\en", 6);
        close(parent_fd[1]);
        char buf[512];
        read(child_fd[0],buf,512);
        close(child_fd[0]);
        printf("%d: received ",getpid());
        printf("%s\n",buf);
    }
    exit();
}