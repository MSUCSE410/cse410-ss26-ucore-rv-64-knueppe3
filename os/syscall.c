#include "syscall.h"
#include "console.h"
#include "const.h"
#include "defs.h"
#include "loader.h"
#include "log.h"
#include "proc.h"
#include "riscv.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
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

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
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

uint64 sys_task_info(uint64 addr)
{
	TaskInfo ti;

	ti.status = curr_proc()->taskinfo.status;
	for (int i = 0; i < MAX_SYSCALL_NUM; ++i)
	{
		ti.syscall_time[i] = curr_proc()->taskinfo.syscall_time[i];
		if (ti.syscall_time[i] > 0)
		{
			debugf("id %d times %d", i, ti.syscall_time[i]);
		}
	}

	uint64 sec = get_cycle() / CPU_FREQ;
	uint64 usec = (get_cycle() % CPU_FREQ) * 1000000 / CPU_FREQ;

	ti.time = (sec * 1000 + usec / 1000) - curr_proc()->taskinfo.time;
	debugf("time %d", ti.time);
	debugf("addr %p", useraddr(curr_proc()->pagetable, addr));
	debugf("time %d", ti.time);

	copyout(curr_proc()->pagetable, addr, (char*) &ti, (uint64) sizeof(TaskInfo));

	return 0; 
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

int sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	debugf("sys_mmap start = %d len = %d port = %d", start, len, port);

	if (len < 1 || len > 1024*1024*1024 || (port & ~0x7) != 0 || (port & 0x7) == 0)
	{
		infof("Some error with length or port");
		return -1;
	}

	uint64 offset = start & 0xFFF;
	if (offset != 0)
	{
		return -1;
	}

	uint64 aligned_length = PGROUNDUP(len);

	infof("Mapping pages");
	while (aligned_length > 0)
	{
		void* pa = kalloc();
		if ((uint64) pa == 0)
		{
			debugf("No physical memory");
			return -1;
		}
		if (mappages(curr_proc()->pagetable, start, PGSIZE, (uint64) pa, PTE_U | (port << 1)) != 0)
		{
			debugf("Failed to map a page");
			return -1;
		}
		aligned_length -= PGSIZE;
		start += PGSIZE;
	}	
	
	debugf("mmap succeeded");
	return 0;
}

int sys_munmap(uint64 start, uint64 len)
{
	debugf("sys_munmap start = %d len = %d", start, len);

	pagetable_t table = curr_proc()->pagetable; // pagetable
	
	// if start isn't aligned with a page start
	if (start % PGSIZE != 0)
	{
		return -1;
	}
	
	int num_pages = PGROUNDUP(len) / PGSIZE;

	for (uint64 page = start; page < start + num_pages * PGSIZE; page += PGSIZE)
	{		
		if(useraddr(table, page) == 0)
		{
			debugf("munmap: not mapped");
			return -1;
		}
		uvmunmap(table, page, 1, 0);
	}

	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	struct proc* p = curr_proc();
	char filename[MAX_STR_LEN];
	
	int size = copyinstr(p->pagetable, filename, va, MAX_STR_LEN);
	if (size < 0)
	{
		return -1;
	}
	int pid = spawn(filename);
	return pid;
}

uint64 sys_set_priority(long long prio){
    // TODO: your job is to complete the sys call
    return -1;
}


extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	++curr_proc()->taskinfo.syscall_time[id];

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_task_info:
		ret = sys_task_info(args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
