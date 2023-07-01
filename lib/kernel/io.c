#include "io.h"

// 端口写一个字节
inline void outb(uint16_t port, uint8_t value) {
  asm volatile("outb %1, %0" : : "dN"(port), "a"(value));
}

// 端口读一个字节
inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  asm volatile("inb %1,%0" : "=a"(ret) : "dN"(port));
  return ret;
}

// 端口读两个字节
inline uint16_t inw(uint16_t port) {
  uint16_t ret;
  asm volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
  return ret;
}

/* 将addr处起始的word_cnt个字写入端口port */
inline void outsw(uint16_t port, const void* addr, uint32_t word_cnt) {
  asm volatile("cld; rep outsw" : "+S"(addr), "+c"(word_cnt) : "d"(port));
}

/* 将从端口port读入的word_cnt个字写入addr */
inline void insw(uint16_t port, void* addr, uint32_t word_cnt) {
  asm volatile("cld; rep insw"
               : "+D"(addr), "+c"(word_cnt)
               : "d"(port)
               : "memory");
}