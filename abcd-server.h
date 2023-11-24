#ifndef __BAD_H__
#define __BAD_H__

#include <stdbool.h>
#include <termios.h>

#include "abcd.h"

void watch_add_readfd(int fd, int (*cb)(int, void*), void *data);
void watch_del_readfd(int fd);
int watch_add_quit(int (*cb)(int, void*), void *data);
void watch_timer_add(int timeout_ms, void (*cb)(void *), void *data);
void watch_quit(void);
int watch_run(void);

int tty_open(const char *tty, struct termios *old);

void abcd_send_buf(int type, size_t len, const void *buf);
#define abcd_send(type) abcd_send_buf(type, 0, NULL)

#endif
