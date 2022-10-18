#pragma once

#include <signal.h>
#include <sys/wait.h>

typedef void (*nokill_t)( int );
extern nokill_t nokill_handler;

void nokill_init();
void nokill_clear();

