#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if(argc!=1) {
        printf("sleep: usage sleep [n]")
        exit();
    } 
    sleep(atoi(argv[1]));
    exit();
}