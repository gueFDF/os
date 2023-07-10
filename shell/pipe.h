#ifndef SHELL_PIPE
#define SHELL_PIPE
#include "global.h"
#include "stdio.h"
bool is_pipe(uint32_t local_fd);
int32_t sys_pipe(int32_t pipefd[2]);
uint32_t pipe_read(int32_t fd, void* buf, uint32_t count);
uint32_t pipe_write(int32_t fd, const void* buf, uint32_t count);
#endif /* SHELL_PIPE */
