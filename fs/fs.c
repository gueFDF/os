#include "fs.h"

#include "debug.h"
#include "dir.h"
#include "inode.h"
#include "list.h"
#include "memory.h"
#include "stdio_kernel.h"
#include "string.h"
#include "super_block.h"
#include "types.h"

struct partition* cur_part;  // 默认情况下操作的是哪一个分区

/*文件表*/
struct file file_table[MAX_FILE_OPEN];

/*从文件表file_table中获取一个空闲位，成功返回下标，失败返回-1*/
int32_t get_free_solt_in_global() {
  uint32_t fd_idx = 3;
  while (fd_idx < MAX_FILE_OPEN) {
    if (file_table[fd_idx].fd_inode == NULL) {
      break;
    }
    fd_idx++;
  }
  if (fd_idx == MAX_FILE_OPEN) {
    printk("exceed max open files\n");
    return -1;
  }
  return fd_idx;
}

/*分配一个节点*/
int32_t inode_bitmap_alloc(struct partition* part) {
  int32_t bit_idx = bitmap_scan(&part->inode_bitmap, 1);
  if (bit_idx == -1) {
    return -1;
  }
  bitmap_set(&part->inode_bitmap, bit_idx, 1);
  return bit_idx;
}

// 分配一个扇区，返回其扇区地址
int32_t block_bitmap_alloc(struct partition* part) {
  int32_t bit_idx = bitmap_scan(&part->block_bitmap, 1);
  if (bit_idx == -1) {
    return -1;
  }
  bitmap_set(&part->block_bitmap, bit_idx, 1);
  return (part->sb->data_start_lba + bit_idx);
}

/*将内存中bitmap第bit_idx位所在的512字节同步到硬盘*/
void bitmap_sync(struct partition* part, uint32_t bit_idx, uint8_t btmp) {
  uint32_t off_sec = bit_idx / 4096;         // 该位扇区偏移量
  uint32_t off_size = off_sec * BLOCK_SIZE;  // 该位字节偏移量

  uint32_t sec_lba;
  uint8_t* bitmap_off;
  /*需要被同步到硬盘的位图只有inode_bitmap和block_map*/
  switch (btmp) {
    case INODE_BITMAP:
      sec_lba = part->sb->inode_bitmap_lba + off_sec;
      bitmap_off = part->inode_bitmap.bits + off_size;
      break;
    case BITMAP_MASK:
      sec_lba = part->sb->block_bitmap_lba + off_sec;
      bitmap_off = part->block_bitmap.bits + off_size;
      break;
  }
  ide_write(part->my_disk, sec_lba, bitmap_off, 1);
}

/*在分区链表中找到名为part_name的分区，并将其指针赋给cur_part*/
static bool mount_partition(struct list_elem* pelem, int arg) {
  char* part_name = (char*)arg;
  struct partition* part = elem2entry(struct partition, part_tag, pelem);
  if (!strcmp(part->name, part_name)) {
    cur_part = part;
    struct disk* hd = cur_part->my_disk;

    /*sd_buf用来存储从硬盘上读入的超级块*/
    struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);
    /* 在内存中创建分区 cur_part 的超级块 */
    cur_part->sb = (struct super_block*)sys_malloc(sizeof(struct super_block));
    if (cur_part->sb == NULL) {
      PANIC("alloc memory failed!");
    }
    /*读入超级块*/
    memset(sb_buf, 0, SECTOR_SIZE);
    ide_read(hd, cur_part->start_lba + 1, sb_buf, 1);
    /* 把 sb_buf 中超级块的信息复制到分区的超级块 sb 中 */
    memcpy(cur_part->sb, sb_buf, sizeof(struct super_block));

    /*读取块位图*/
    cur_part->block_bitmap.bits =
        (uint8_t*)sys_malloc(sb_buf->block_bitmap_sects * SECTOR_SIZE);
    if (cur_part->block_bitmap.bits == NULL) {
      PANIC("alloc memory failed!");
    }
    cur_part->block_bitmap.btmp_bytes_len =
        sb_buf->block_bitmap_sects * SECTOR_SIZE;
    // 从磁盘中读取块位图
    ide_read(hd, sb_buf->block_bitmap_lba, cur_part->block_bitmap.bits,
             sb_buf->block_bitmap_sects);

    /**********将硬盘上的 inode 位图读入到内存************/
    cur_part->inode_bitmap.bits =
        (uint8_t*)sys_malloc(sb_buf->inode_bitmap_sects * SECTOR_SIZE);
    if (cur_part->inode_bitmap.bits == NULL) {
      PANIC("alloc memory failed!");
    }
    cur_part->inode_bitmap.btmp_bytes_len =
        sb_buf->inode_bitmap_sects * SECTOR_SIZE;
    ide_read(hd, sb_buf->inode_bitmap_lba, cur_part->inode_bitmap.bits,
             sb_buf->block_bitmap_sects);

    list_init(&cur_part->open_inodes);
    printk("mount %s done!\n", part->name);

    sys_free(sb_buf);
    // 此处是为了配合 list_traversal
    return true;
  }
  return false;
}

int32_t file_create(struct dir* parent_dir, char* filename, uint8_t flag) {
  /*后续操作的公共缓冲区*/
  void* io_buf = sys_malloc(1204);
  if (io_buf == NULL) {
    printk("in file_creat: sys_malloc for io_buf failed\n");
    return -1;
  }

  uint8_t rollback_step = 0;  // 用于回滚操作状态

  /*为新文件分配inode*/
  int32_t inode_no = inode_bitmap_alloc(cur_part);
  if (inode_no == -1) {
    printk("in file_creat: allocate inode failed\n");
    return -1;
  }

  /* 此 inode 要从堆中申请内存,不可生成局部变量(函数退出时会释放)
   * 因为 file_table 数组中的文件描述符的 inode 指针要指向它 */
  struct inode* new_file_inode =
      (struct inode*)sys_malloc(sizeof(struct inode));
  if (new_file_inode == NULL) {
    printk("file_create: sys_malloc for inode failded\n");
    rollback_step = 1;
    goto rollback;
  }
  inode_init(inode_no, new_file_inode);  // 初始化inode节点

  int fd_idx = get_free_solt_in_global();
  if (fd_idx == -1) {
    printk("exceed max open files\n");
    rollback_step = 2;
    goto rollback;
  }

  file_table[fd_idx].fd_inode = new_file_inode;
  file_table[fd_idx].fd_pos = 0;
  file_table[fd_idx].fd_flag = flag;
  file_table[fd_idx].fd_inode->write_deny = false;
  struct dir_entry new_dir_entry;
  memset(&new_dir_entry, 0, sizeof(struct dir_entry));
  create_dir_entry(filename, inode_no, FT_REGULAR, &new_dir_entry);

  /*同步内存数据到磁盘*/
  if (!sync_dir_entry(parent_dir, &new_dir_entry, io_buf)) {
    printk("sync dir_entry to disk failed\n");
    rollback_step = 3;
    goto rollback;
  }

  memset(io_buf, 0, 1024);
  /*将父目录的i节点的内容同步到硬盘*/
  inode_sync(cur_part, parent_dir->inode, io_buf);

  memset(io_buf, 0, 1024);
  /*将新创建文件的 i 结点内容同步到硬盘*/
  inode_sync(cur_part, new_file_inode, io_buf);

  /*将inode_bitmap位图同步到硬盘*/
  bitmap_sync(cur_part, inode_no, INODE_BITMAP);

  /*将创建的文件inode节点添加到open_inodes链表*/

  list_push(&cur_part->open_inodes, &new_file_inode->inode_tag);
  new_file_inode->i_open_cnts = 1;
  sys_free(io_buf);

  return pcb_fd_install(fd_idx);
rollback:
  switch (rollback_step) {
    case 3:
      memset(&file_table[fd_idx], 0, sizeof(struct file));
    case 2:
      sys_free(new_file_inode);
    case 1:
      bitmap_set(&cur_part->inode_bitmap, inode_no, 0);
      break;
  }
  sys_free(io_buf);
  return -1;
}

/*格式化分区数据，也就是初始化分区的元数据，创建文件系统*/
static void partition_format(struct partition* part) {
  /* blocks_bitmap_init(为方便实现,一个块大小是一扇区)*/
  // 计算每一个模块所要占用的扇区数
  uint32_t boot_sector_sects = 1;
  uint32_t super_block_sects = 1;
  uint32_t inode_bitmap_sects =
      DIV_ROUND_UP(MAX_FILES_PER_PART, BITS_PER_SECTOR);

  uint32_t inode_table_sects =
      DIV_ROUND_UP((sizeof(struct inode) * MAX_FILES_PER_PART), SECTOR_SIZE);

  uint32_t used_sects = boot_sector_sects + super_block_sects +
                        inode_bitmap_sects + inode_table_sects;
  uint32_t free_sects = part->sec_cnt - used_sects;

  /************** 简单处理块位图占据的扇区数 ***************/
  /*块位图是放在可用区域内的*/
  uint32_t block_bitmap_sects;
  block_bitmap_sects = DIV_ROUND_UP(free_sects, BITS_PER_SECTOR);
  uint32_t block_bitmap_bit_len =
      free_sects - block_bitmap_sects;  // 除去块位图占用的扇区
  // 二次计算
  block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len, BITS_PER_SECTOR);

  /*超级块初始化*/
  struct super_block sb;
  sb.magic = 0x19590318;
  sb.sec_cnt = part->sec_cnt;
  sb.inode_cnt = MAX_FILES_PER_PART;
  sb.part_lba_base = part->start_lba;

  sb.block_bitmap_lba = sb.part_lba_base + 2;  // 第一个引导块，第二个为超级快
  sb.block_bitmap_sects = block_bitmap_sects;

  sb.inode_bitmap_lba = sb.block_bitmap_lba + sb.block_bitmap_sects;
  sb.inode_bitmap_sects = inode_bitmap_sects;

  sb.inode_table_lba = sb.inode_bitmap_lba + sb.inode_bitmap_sects;
  sb.inode_table_sects = inode_table_sects;

  sb.data_start_lba = sb.inode_table_lba + sb.inode_table_sects;
  sb.root_inode_no = 0;
  sb.dir_entry_size = sizeof(struct dir_entry);

  printk("%s info:\n", part->name);

  printk("magic : 0x%x\n", sb.magic);
  printk("part_lba_base : 0x%x\n", sb.part_lba_base);
  printk("all_sectors : 0x%x\n", sb.sec_cnt);
  printk("inode_cnt : 0x%x\n", sb.inode_cnt);
  printk("block_bitmap_lba : 0x%x\n", sb.block_bitmap_lba);
  printk("block_bitmap_sectors : 0x%x\n ", sb.block_bitmap_sects);
  printk("inode_bitmap_lba : 0x%x\n", sb.inode_bitmap_lba);
  printk("inode_bitmap_sectors : 0x%x\n", sb.inode_bitmap_sects);
  printk("inode_table_lba : 0x%x\n", sb.inode_table_lba);
  printk("inode_table_sectors : 0x%x\n", sb.inode_table_sects);
  printk("data_start_lba : 0x%x\n", sb.data_start_lba);

  struct disk* hd = part->my_disk;

  /*1. 将超级块写入本分区的1扇区*/
  ide_write(hd, part->start_lba + 1, &sb, 1);
  printk("super_block_lba:0x%x\n", part->start_lba + 1);

  /*找数据量最大的元信息，用其尺寸做存储缓冲区(存放位图)*/
  uint32_t buf_size =
      (sb.block_bitmap_sects >= sb.inode_bitmap_sects ? sb.block_bitmap_sects
                                                      : sb.inode_bitmap_sects);

  buf_size = (buf_size >= sb.inode_table_sects ? buf_size : inode_table_sects) *
             SECTOR_SIZE;

  uint8_t* buf = (uint8_t*)sys_malloc(buf_size);

  /*2. 将块位图初始化并写入sb.block_bitmap_lab*/
  buf[0] |= 0x01;  // 将第0个块预留给根目录，位图中先占位置
  uint32_t block_bitmap_last_byte = block_bitmap_bit_len / 8;
  uint8_t block_bitmap_last_bit = block_bitmap_bit_len % 8;
  uint32_t last_size = SECTOR_SIZE - (block_bitmap_last_byte % SECTOR_SIZE);

  // 最后一个位图多于空间全置1(先忽略bit)
  memset(&buf[block_bitmap_last_byte], 0xff, last_size);
  /*在补上bit*/
  uint8_t bit_idx = 0;
  while (bit_idx <= block_bitmap_last_bit) {
    buf[block_bitmap_last_byte] &= ~(1 << bit_idx++);
  }

  ide_write(hd, sb.block_bitmap_lba, buf, sb.block_bitmap_sects);

  // 3 将 inode 位图初始化并写入 sb.inode_bitmap_lba
  // 先清空缓冲区
  memset(buf, 0, buf_size);
  buf[0] |= 0x1;
  // inode一个4096个刚好占用一个扇区
  ide_write(hd, sb.inode_bitmap_lba, buf, sb.inode_bitmap_sects);

  // 4 将 inode 数组初始化并写入 sb.inode_table_lba
  memset(buf, 0, buf_size);
  // 写入第0个inode(第0个指向根目录)
  struct inode* i = (struct inode*)buf;
  i->i_size = sb.dir_entry_size * 2;  // .和..
  i->i_no = 0;  // 根目录占inode数组中的第0个inode
  i->i_sectors[0] = sb.data_start_lba;

  ide_write(hd, sb.inode_table_lba, buf, sb.inode_table_sects);

  // 5 将根目录写入 sb.data_start_lba
  // 初始化根目录的两个目录项 . 和 ..
  memset(buf, 0, buf_size);
  struct dir_entry* p_de = (struct dir_entry*)buf;
  // 初始化.
  memcpy(p_de->filename, ".", 1);
  p_de->i_no = 0;
  p_de->f_type = FT_DIRECTORY;
  p_de++;
  // 初始化 ..
  memcpy(p_de->filename, "..", 2);
  p_de->i_no = 0;  // 根目录的父目录依然是根目录自己
  p_de->f_type = FT_DIRECTORY;
  // 第一个块已经预留给根目录
  ide_write(hd, sb.data_start_lba, buf, 1);
  printk("root_dir_lba:0x%x\n", sb.data_start_lba);
  printk("%s format done\n", part->name);
  sys_free(buf);
}

/*将最上层的路径名称解析出来 只会解析一层*/
static char* path_parse(char* pathname, char* name_store) {
  if (pathname[0] == '/') {  // 根目录不需要单独解析
    while (*(++pathname) == '/')
      ;
  }

  /*开始一般的路径解析*/
  while (*pathname != '/' && *pathname != 0) {
    *name_store++ = *pathname++;
  }

  if (pathname[0] == 0) {  // 若路径字符串为空,则返回 NULL
    return NULL;
  }

  return pathname;
}

/*返回路径深度，比如/a/b/c，深度为3*/
int32_t path_depth_cnt(char* pathname) {
  ASSERT(pathname != NULL);
  char* p = pathname;
  char name[MAX_FILE_NAME_LEN];
  uint32_t depth = 0;

  /*解析路径，从中拆分出各级名称*/
  p = path_parse(p, name);
  while (name[0]) {
    depth++;
    memset(name, 0, MAX_FILE_NAME_LEN);
    if (p) {
      p = path_parse(p, name);
    }
  }

  return depth;
}

/*搜索文件 pathname,若找到则返回其 inode 号,否则返回-1 */
static int search_file(const char* pathname,
                       struct path_search_record* searched_record) {
  if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") ||
      !strcmp(pathname, "/..")) {
    searched_record->parent_dir = &root_dir;
    searched_record->file_type = FT_DIRECTORY;
    searched_record->searched_path[0] = 0;  // 搜索路径为空
    return 0;
  }

  uint32_t path_len = strlen(pathname);
  /* 保证 pathname 至少是这样的路径/x,且小于最大长度 */
  ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);

  char* sub_path = (char*)pathname;

  struct dir* parent_dir = &root_dir;
  struct dir_entry dir_e;

  char name[MAX_FILE_NAME_LEN] = {0};  // 存放每次解析出来的名字

  searched_record->parent_dir = parent_dir;
  searched_record->file_type = FT_UNKNOWN;
  uint32_t parent_inode_no = 0;  // 父目录的inode号

  sub_path = path_parse(sub_path, name);
  while (name[0]) {
    ASSERT(strlen(searched_record->searched_path) < 512);
    // 记录已经存在的父目录
    strcat(searched_record->searched_path, "/");
    strcat(searched_record->searched_path, name);

    /*在所在的目录中查找文件*/
    if (search_dir_entry(cur_part, parent_dir, name, &dir_e)) {
      memset(name, 0, MAX_FILE_NAME_LEN);
      if (sub_path) {
        sub_path = path_parse(sub_path, name);
      }

      if (FT_DIRECTORY == dir_e.f_type) {
        parent_inode_no = parent_dir->inode->i_no;
        dir_close(parent_dir);
        parent_dir = dir_open(cur_part, dir_e.i_no);  // 更新父目录
        searched_record->parent_dir = parent_dir;
        continue;
      } else if (FT_REGULAR == dir_e.f_type) {
        searched_record->file_type = FT_REGULAR;
        return dir_e.i_no;
      }
    } else {
      return -1;
    }
  }

  dir_close(searched_record->parent_dir);
  /* 保存被查找目录的直接父目录 */
  searched_record->parent_dir = dir_open(cur_part, parent_inode_no);
  searched_record->file_type = FT_DIRECTORY;
  return dir_e.i_no;
}

/* 在磁盘上搜索文件系统,若没有则格式化分区创建文件系统 */
void filesys_init() {
  uint8_t channel_no = 0, dev_no = 0, part_idx = 0;
  /*sb_buf用来存储从硬盘上读入的超级块*/
  struct super_block* sb_buf =
      (struct super_block*)sys_malloc(sizeof(struct super_block));

  if (sb_buf == NULL) {
    PANIC("filesys_init alloc memory failed!...\n");
  }
  printk("searching filesystem......\n");
  while (channel_no < channel_cnt) {
    dev_no = 0;
    while (dev_no < 2) {
      if (dev_no == 0) {  // 跨过主盘(裸盘)
        dev_no++;
        continue;
      }
      struct disk* hd = &channels[channel_no].devices[dev_no];
      struct partition* part = hd->prim_parts;
      while (part_idx < 12) {  // 4(主)+8(逻辑)
        if (part_idx == 4) {
          part = hd->logic_parts;
        }
        if (part->sec_cnt != 0) {
          memset(sb_buf, 0, SECTOR_SIZE);
          // 读取超级块，根据魔术来判断是否存在文件系统
          ide_read(hd, part->start_lba + 1, sb_buf, 1);
          if (sb_buf->magic == 0x19590318) {
            printk("%s has filesystem\n", part->name);
          } else {
            printk("formatting %s`s partition %s......\n", hd->name,
                   part->name);
            partition_format(part);
          }
        }
        part_idx++;
        part++;
      }
      dev_no++;  // 下一个磁盘
    }
    channel_no++;  // 下一个通道
  }
  sys_free(sb_buf);

  /*确认默认操作的分区*/
  char default_part[8] = "sdb1";

  /*挂载分区*/
  list_traversal(&partition_list, mount_partition, (int)default_part);
}
