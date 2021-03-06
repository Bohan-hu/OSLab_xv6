#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// Buddy allocator

static int nsizes;     // the number of entries in bd_sizes array

#define LEAF_SIZE     16                         // The smallest block size 最小的块大小
#define MAXSIZE       (nsizes-1)                 // Largest index in bd_sizes array bd_sizes数组中最大的索引号
#define BLK_SIZE(k)   ((1L << (k)) * LEAF_SIZE)  // Size of block at size k 
#define HEAP_SIZE     BLK_SIZE(MAXSIZE) 
#define NBLK(k)       (1 << (MAXSIZE-k))         // Number of block at size k
#define ROUNDUP(n,sz) (((((n)-1)/(sz))+1)*(sz))  // Round up to the next multiple of sz

typedef struct list Bd_list;
// 核心思想就是给出地址p，找到在第k组中地址p对应的块号，进行标记
// 如果是申请N字节内存，从第一个大小大于N的块号开始找，找到有空闲链表的之后，如果这个块比要申请的块2倍还大，则需要对该块进行分割
// The allocator has sz_info for each size k. Each sz_info has a free
// list, an array alloc to keep track which blocks have been
// allocated, and an split array to to keep track which blocks have
// been split.  The arrays are of type char (which is 1 byte), but the
// allocator uses 1 bit per block (thus, one char records the info of
// 8 blocks).
struct sz_info {
  Bd_list free; //空闲列表
  char *alloc;  //已经分配的（1个bit代表一位）
  char *split;  //被拆分的(1个bit代表一位)
};
typedef struct sz_info Sz_info;

static Sz_info *bd_sizes;   //数组，大小为nsizes
static void *bd_base;   // buddy管理器管理的内存基地址 start address of memory managed by the buddy allocator
static struct spinlock lock;

//索引为n的bd_sizes里有2^n个块

// Return 1 if bit at position index in array is set to 1
int bit_isset(char *array, int index) {
  char b = array[index/8];
  char m = (1 << (index % 8));
  return (b & m) == m;
}

// Set bit at position index in array to 1
void bit_set(char *array, int index) {
  char b = array[index/8];
  char m = (1 << (index % 8));
  array[index/8] = (b | m);
}

// Clear bit at position index in array
void bit_clear(char *array, int index) {
  char b = array[index/8];
  char m = (1 << (index % 8));
  array[index/8] = (b & ~m);
}

// Print a bit vector as a list of ranges of 1 bits
void
bd_print_vector(char *vector, int len) {
  int last, lb;
  
  last = 1;
  lb = 0;
  for (int b = 0; b < len; b++) {
    if (last == bit_isset(vector, b))
      continue;
    if(last == 1)
      printf(" [%d, %d)", lb, b);
    lb = b;
    last = bit_isset(vector, b);
  }
  if(lb == 0 || last == 1) {
    printf(" [%d, %d)", lb, len);
  }
  printf("\n");
}

// Print buddy's data structures
void
bd_print() {
  for (int k = 0; k < nsizes; k++) {
    printf("size %d (blksz %d nblk %d): free list: ", k, BLK_SIZE(k), NBLK(k));
    lst_print(&bd_sizes[k].free);
    printf("  alloc:");
    bd_print_vector(bd_sizes[k].alloc, NBLK(k));
    if(k > 0) {
      printf("  split:");
      bd_print_vector(bd_sizes[k].split, NBLK(k));
    }
  }
}

// What is the first k such that 2^k >= n?
//找到第一个k使得2^k >= n
int
firstk(uint64 n) {
  int k = 0;
  uint64 size = LEAF_SIZE;

  while (size < n) {
    k++;
    size *= 2;
  }
  return k;
}

// Compute the block index for address p at size k
// 计算：对于大小为k的组，其下标是多少
int
blk_index(int k, char *p) {
  int n = p - (char *) bd_base;
  return n / BLK_SIZE(k);
}

// Convert a block index at size k back into an address
// 把大小为k的块转换为地址
void *addr(int k, int bi) {
  int n = bi * BLK_SIZE(k);
  return (char *) bd_base + n;
}

// allocate nbytes, but malloc won't return anything smaller than LEAF_SIZE
// 分配n个byte，但是不会返回任何比块大小少的块
void *
bd_malloc(uint64 nbytes)
{
  int fk, k;

  acquire(&lock);

  // Find a free block >= nbytes, starting with smallest k possible
  // 找到第一个空闲的块
  fk = firstk(nbytes);
  for (k = fk; k < nsizes; k++) {
    if(!lst_empty(&bd_sizes[k].free))
      break;
  }
  // 没有找到空闲块
  if(k >= nsizes) { // No free blocks?
    release(&lock);
    return 0;
  }

  // 从free表中找到第一个大小大于等于当前申请块的block
  // Found a block; pop it and potentially split it.
  // 并且弹出，得到这个空闲的地址
  char *p = lst_pop(&bd_sizes[k].free);
  // Set the bit allocated
  bit_set(bd_sizes[k].alloc, blk_index(k, p));    
  for(; k > fk; k--) {
    // split a block at size k and mark one half allocated at size k-1
    // 把k分成两半，含有该块的一半分配给下一级，另一半加入下一级的空闲表
    // 在下一级中，也是同样的操作
    // and put the buddy on the free list at size k-1
    char *q = p + BLK_SIZE(k-1);   // p's buddy
    bit_set(bd_sizes[k].split, blk_index(k, p));
    bit_set(bd_sizes[k-1].alloc, blk_index(k-1, p));
    lst_push(&bd_sizes[k-1].free, q);
  }
  release(&lock);
  return p;
}

// Find the size of the block that p points to.
int
size(char *p) {
  for (int k = 0; k < nsizes; k++) {
    if(bit_isset(bd_sizes[k+1].split, blk_index(k+1, p))) {
      return k;
    }
  }
  return 0;
}

// Free memory pointed to by p, which was earlier allocated using
// bd_malloc.
void
bd_free(void *p) {
  void *q;
  int k;

  acquire(&lock);
  for (k = size(p); k < MAXSIZE; k++) {
    // 从大小k开始向上寻找
    int bi = blk_index(k, p);
    int buddy = (bi % 2 == 0) ? bi+1 : bi-1;  //伙伴的下标
    bit_clear(bd_sizes[k].alloc, bi);  // free p at size k  //设为没有分配
    if (bit_isset(bd_sizes[k].alloc, buddy)) {  // is buddy allocated?  //如果伙伴被分配了
      break;   // break out of loop
    }
    // budy is free; merge with buddy
    // 伙伴没有被分配，可以将两块内存合并起来
    q = addr(k, buddy);   
    lst_remove(q);    // remove buddy from free list  //把第k层的这个伙伴节点从链表中摘除
    if(buddy % 2 == 0) {  // 合并成更大的节点，地址从偶数块开始
      p = q;
    }
    // at size k+1, mark that the merged buddy pair isn't split
    // anymore
    // Split标记清除
    bit_clear(bd_sizes[k+1].split, blk_index(k+1, p));
  }
  lst_push(&bd_sizes[k].free, p);
  release(&lock);
}

// Compute the first block at size k that doesn't contain p
int
blk_index_next(int k, char *p) {
  int n = (p - (char *) bd_base) / BLK_SIZE(k);
  if((p - (char*) bd_base) % BLK_SIZE(k) != 0)
      n++;
  return n ;
}

int
log2(uint64 n) {
  int k = 0;
  while (n > 1) {
    k++;
    n = n >> 1;
  }
  return k;
}

// Mark memory from [start, stop), starting at size 0, as allocated. 
// 将[start, stop)区域的内存标记为已经被分配
void
bd_mark(void *start, void *stop)
{
  int bi, bj;

  if (((uint64) start % LEAF_SIZE != 0) || ((uint64) stop % LEAF_SIZE != 0))
    // 内存地址没有对齐
    panic("bd_mark");

  for (int k = 0; k < nsizes; k++) {
    // 对每一组进行标记
    // 找出对于该组，块的起始索引和终止索引
    bi = blk_index(k, start);
    bj = blk_index_next(k, stop);
    for(; bi < bj; bi++) {
      if(k > 0) {
        // if a block is allocated at size k, mark it as split too.
        // 某个块被分配了，那么他一定是被分割了
        bit_set(bd_sizes[k].split, bi);
      }
      bit_set(bd_sizes[k].alloc, bi);
    }
  }
}


// Buddy: 一个被分配块，平均拆分成两个大小是原来一半的块单元，这两个块单元互为伙伴
// 块B的伙伴必须大小和B一样大，并且内存地址相邻
// 块单元在内存中的地址需要能被自己的大小整除
// If a block is marked as allocated and the buddy is free, put the
// buddy on the free list at size k.
int
bd_initfree_pair(int k, int bi) {
  //找到伙伴的index，处在一组中
  int buddy = (bi % 2 == 0) ? bi+1 : bi-1;
  int free = 0;
  // 伙伴只有一个被分配，则另一个被挂到了空闲链表上
  if(bit_isset(bd_sizes[k].alloc, bi) !=  bit_isset(bd_sizes[k].alloc, buddy)) {
    // one of the pair is free
    // bi和他的buddy有且仅有一个是free的
    free = BLK_SIZE(k); //free的大小是k层的block size
    if(bit_isset(bd_sizes[k].alloc, bi))  //如果buddy是free的
      // list的地址，就是free空间开始的地址，因为是free的，所以可以随便搞
      // 把k层的放到free表中
      lst_push(&bd_sizes[k].free, addr(k, buddy));   // put buddy on free list
    else
      lst_push(&bd_sizes[k].free, addr(k, bi));      // put bi on free list
  }
  // 返回空余的地址
  return free;
}
  
// Initialize the free lists for each size k.  For each size k, there
// are only two pairs that may have a buddy that should be on free list:
// bd_left and bd_right.
// TODO: buddy的定义是什么？
int
bd_initfree(void *bd_left, void *bd_right) {
  int free = 0;

  for (int k = 0; k < MAXSIZE; k++) {   // skip max size
    int left = blk_index_next(k, bd_left);
    int right = blk_index(k, bd_right);
    free += bd_initfree_pair(k, left);
    if(right <= left)
      continue;
    free += bd_initfree_pair(k, right);
  }
  return free;
}

// Mark the range [bd_base,p) as allocated
// 需要把该部分内存标记为已经分配（被分配器本身的数据结构所占用了）
int
bd_mark_data_structures(char *p) {
  int meta = p - (char*)bd_base;
  printf("bd: %d meta bytes for managing %d bytes of memory\n", meta, BLK_SIZE(MAXSIZE));
  bd_mark(bd_base, p);
  return meta;
}

// Mark the range [end, HEAPSIZE) as allocated
int
bd_mark_unavailable(void *end, void *left) {
  int unavailable = BLK_SIZE(MAXSIZE)-(end-bd_base);
  if(unavailable > 0)
    unavailable = ROUNDUP(unavailable, LEAF_SIZE);
  printf("bd: 0x%x bytes unavailable\n", unavailable);

  void *bd_end = bd_base+BLK_SIZE(MAXSIZE)-unavailable;
  bd_mark(bd_end, bd_base+BLK_SIZE(MAXSIZE));
  return unavailable;
}

// Initialize the buddy allocator: it manages memory from [base, end).
// buddy分配器管理的是从base到end的内存空间
void
bd_init(void *base, void *end) {
  char *p = (char *) ROUNDUP((uint64)base, LEAF_SIZE); // 把基地址对齐到LEAF_SIZE的整数倍
  int sz;

  initlock(&lock, "buddy");
  bd_base = (void *) p;   //基地址就是p，已经对齐过了

  // compute the number of sizes we need to manage [base, end)
  // 计算需要的不同的块大小数量
  nsizes = log2(((char *)end-p)/LEAF_SIZE) + 1;
  if((char*)end-p > BLK_SIZE(MAXSIZE)) {
    nsizes++;  // round up to the next power of 2
  }

  printf("bd: memory sz is %d bytes; allocate an size array of length %d\n",
         (char*) end - p, nsizes);

  // allocate bd_sizes array
  // 分配bd_sizes数组，占用一部分的空间（占用的是需要分配的内存空间）
  bd_sizes = (Sz_info *) p;
  p += sizeof(Sz_info) * nsizes;
  memset(bd_sizes, 0, sizeof(Sz_info) * nsizes);

  // initialize free list and allocate the alloc array for each size k
  // 对于每个数组下标K，对应管理的片的大小是2^K，对其进行初始化
  for (int k = 0; k < nsizes; k++) {
    lst_init(&bd_sizes[k].free);    //初始化free_list为空
    sz = sizeof(char)* ROUNDUP(NBLK(k), 8)/8;   //一个char能存8个块的状态，所以除以8
    bd_sizes[k].alloc = p;    //alloc数组就从p开始，alloc是位数组
    memset(bd_sizes[k].alloc, 0, sz);
    p += sz;  //p移动到下一个块
  }

  // allocate the split array for each size k, except for k = 0, since
  // we will not split blocks of size k = 0, the smallest size.
  for (int k = 1; k < nsizes; k++) {
    sz = sizeof(char)* (ROUNDUP(NBLK(k), 8))/8;
    bd_sizes[k].split = p;
    memset(bd_sizes[k].split, 0, sz);
    p += sz;
  }

  //把p移动到空间的末尾
  p = (char *) ROUNDUP((uint64) p, LEAF_SIZE);

  // done allocating; mark the memory range [base, p) as allocated, so
  // that buddy will not hand out that memory.
  // 至此，整个分配器占用了前面整个部分的内存空间，标记为已分配，meta是占用的内存单元数量
  int meta = bd_mark_data_structures(p);
  
  // mark the unavailable memory range [end, HEAP_SIZE) as allocated,
  // so that buddy will not hand out that memory.
  // 剩余未对齐空间，也是不可以使用的，标记为已经分配
  int unavailable = bd_mark_unavailable(end, p);
  void *bd_end = bd_base+BLK_SIZE(MAXSIZE)-unavailable;
  
  // initialize free lists for each size k
  int free = bd_initfree(p, bd_end);
  // check if the amount that is free is what we expect
  if(free != BLK_SIZE(MAXSIZE)-meta-unavailable) {
    printf("free %d %d\n", free, BLK_SIZE(MAXSIZE)-meta-unavailable);
    panic("bd_init: free mem");
  }
}
