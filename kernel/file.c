//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "stddef.h"
#define INIT_SIZE 64

//bd_malloc不会将内存清零
//bd_print可以打印分配器的状态

#define file2node(node) (file_list*)((void*)(node) - (void*)&(((file_list*)0)->file))

typedef struct _fl {
  struct file file;
  struct list list_head;
} file_list;

struct devsw devsw[NDEV];

struct {
  struct spinlock lock;
  struct list list_head;  //动态分配
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
  //对描述符初始化
  lst_init(&(ftable.list_head));
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;
  acquire(&ftable.lock);
  file_list *node;
  if( (node = bd_malloc(sizeof(file_list))) != 0) {
    // printf("Allocated\n");
    lst_push(&(ftable.list_head), &(node->list_head));
    f = &(node->file);
    f->ref = 1;
    release(&ftable.lock);
    return f;
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)   //需要修改，ff不需要了
{
  acquire(&ftable.lock);    //不能改动
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  file_list* node= file2node(f);
  lst_remove(&(node->list_head));
  release(&ftable.lock);  //不能改动

  //需要对文件进行关闭，因为f->ref = 0了

  if(f->type == FD_PIPE){
    pipeclose(f->pipe, f->writable);
  } else if(f->type == FD_INODE || f->type == FD_DEVICE){
    begin_op(f->ip->dev);
    iput(f->ip);
    end_op(f->ip->dev);
  }
  bd_free(node);
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op(f->ip->dev);
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op(f->ip->dev);

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

