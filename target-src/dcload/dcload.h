#ifndef __DCLOAD_H__
#define __DCLOAD_H__

// For dcload-crt0.s to interface with startup_support.c
void __call_builtin_sh_set_fpscr(unsigned int value);

#endif
