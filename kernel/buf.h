struct buf {
  int valid;   // has data been read from disk? 是否有效
  int disk;    // does disk "own" buf? buf是否被回写到磁盘
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};

