/* x86 architecture timex specifications */
#ifndef _ASM_X86_TIMEX_H
#define _ASM_X86_TIMEX_H

#include <asm/processor.h>
#include <asm/tsc.h>

#ifdef CONFIG_X86_ELAN
#  define PIT_TICK_RATE 1189200 /* AMD Elan has different frequency! */
#else
#  define PIT_TICK_RATE 1193182 /* Underlying HZ */
#endif
#define CLOCK_TICK_RATE	PIT_TICK_RATE

extern int read_current_timer(unsigned long *timer_value);
#define ARCH_HAS_READ_CURRENT_TIMER	1

#endif
