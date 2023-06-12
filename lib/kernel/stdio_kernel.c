#include "print.h"
#include "stdio.h"
#include "global.h"

/* 供内核使用的格式化输出函数 */
void printk(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[1024] = {0};
  vsprintf(buf, format, args);
  va_end(args);
  print_str(buf);
}