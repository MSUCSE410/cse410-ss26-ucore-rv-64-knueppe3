#include "syscall.h"
#include "const.h"
#include "defs.h" 
#include "proc.h"
#include "log.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "types.h"
#include "vm.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 *addr, int _tz) 
{
	// WHY THE HELL IS IT LIKE THIS
	TimeVal timeval;

	uint64 cycle = get_cycle();
	timeval.sec = cycle / CPU_FREQ;
	timeval.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

	tracef("addr %p", useraddr(curr_proc()->pagetable, *addr));

	return copyout(curr_proc()->pagetable, *addr, (char*) &timeval, (uint64) sizeof(TimeVal));
}

uint64 sys_task_info(uint64 *addr)
{
	TaskInfo ti;

	ti.status = curr_proc()->taskinfo.status;
	for (int i = 0; i < MAX_SYSCALL_NUM; ++i)
	{
		ti.syscall_time[i] = curr_proc()->taskinfo.syscall_time[i];
	}
	ti.time = curr_proc()->taskinfo.time; 

	tracef("addr %p", useraddr(curr_proc()->pagetable, *addr));
	debugf("time %d", ti.time);

	return copyout(curr_proc()->pagetable, *addr, (char*) &ti, (uint64) sizeof(TaskInfo));
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	curr_proc()->taskinfo.syscall_time[id]++;

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((uint64 *) &args[0], args[1]);
		break;
	case SYS_task_info:
		ret = sys_task_info((uint64 *) &args[0]);
		break;

	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
