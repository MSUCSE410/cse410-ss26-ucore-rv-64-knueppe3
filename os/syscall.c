#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"
#include "fs.h"
#include "file.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_task_info(uint64 addr)
{
	debugf("sys_taskinfo");

	TaskInfo ti;

	ti.status = curr_proc()->taskinfo.status;
	memmove(ti.syscall_time, curr_proc()->taskinfo.syscall_time, sizeof(ti.syscall_time));

	uint64 sec = get_cycle() / CPU_FREQ;
	uint64 usec = (get_cycle() % CPU_FREQ) * 1000000 / CPU_FREQ;

	ti.time = (sec * 1000 + usec / 1000) - curr_proc()->taskinfo.time;

	copyout(curr_proc()->pagetable, addr, (char*) &ti, (uint64) sizeof(TaskInfo));

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
	uint64 mapped_size = 0;

	infof("Mapping pages with aligned_len %d and page size %d", aligned_length, curr_proc()->max_page);
	while (aligned_length > 0)
	{
		void* pa = kalloc();
		if ((uint64) pa == 0)
		{
			debugf("No physical memory");
			return -1;
		}
		if (mappages(curr_proc()->pagetable, (uint64) start, PGSIZE, (uint64) pa, PTE_U | (port << 1)) < 0)
		{
			debugf("Failed to map a page");
			return -1;
		}
		aligned_length -= PGSIZE;
		start += PGSIZE;
		mapped_size += PGSIZE;
	}	
	if (aligned_length != 0)
	{
		panic("aligned length != 0");
	}
	curr_proc()->max_page += mapped_size / PGSIZE;
	debugf("mmap succeeded with mapped size %d", mapped_size);
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

uint64 sys_wait(int pid, uint64 va)
{
	debugf("sys_wait pid=%d", pid);
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
	debugf("sys_spawn file =%s", filename);
	int pid = spawn(filename);
	return pid;
}

uint64 sys_set_priority(long long prio)
{
	debugf("sys_prio prio=%d", prio);
	if (prio > 1)
	{
		curr_proc()->priority = prio;
		return prio;
	}
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);

	debugf("sys_openat path = %s", path);

	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	debugf("sys_close fd =%d", fd);
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd, uint64 stat)
{

	debugf("sys_fstat fd = %d", fd);
    struct proc *p = curr_proc();

	Stat s;

    // invalid fd
    if (fd < 0 || fd >= FD_BUFFER_SIZE) {
        infof("invalid fd %d", fd);
        return -1;
    }

    struct file *f = p->files[fd];

    // invalid fd
    if (f == NULL) {
        infof("fd %d is not opened", fd);
        return -1;
    }
	if (f->type == FD_NONE)
	{
		infof("fd %d is FD_NONE", fd);
		return -1;
	}

	struct inode * ip;
	if (f->type == FD_INODE) {
		ip = f->ip;
		ivalid(ip);
		s.dev = ip->dev;
		s.ino = ip->inum;
		if (ip->type == 1)
		{
			s.mode = DIR;
		} else if (ip->type == 2)
		{
			s.mode = FILE;
		} else {
			s.mode = -1;
		}
		s.nlink = ip->nlink;
	}
	else if (f->type == FD_STDIO) {
		s.mode = FILE;
		if (f->readable && !f->writable)
		{
			s.ino = 0;
		}
		else if (!f->readable && f->writable)
		{
			s.ino = 1;
		}
		else 
		{
			s.ino = 2;
		}
		s.nlink = 1;
		s.dev = ROOTDEV;
	}
	else {
		errorf("unknown type");
		return -1;
	}
	debugf("dev=%d, mode=%d, ino=%d, nlink=%d", s.dev, s.mode, s.ino, s.nlink);

	return copyout(curr_proc()->pagetable, stat, (char*) &s, (uint64) sizeof(Stat));
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags)
{
	debugf("sys_linkat");
	
	char old[MAXPATH], new[MAXPATH];
	struct inode *ip, *dp;

	if (copyinstr(curr_proc()->pagetable, old, oldpath, MAXPATH) < 0 || copyinstr(curr_proc()->pagetable, new, newpath, MAXPATH) < 0)
		return -1;

	// find existing file
	if ((ip = namei(old)) == 0)
		return -1;

	ivalid(ip);

	// cannot link directories
	if (ip->type == T_DIR) {
		iput(ip);
		return -1;
	}

	// increase link count
	ip->nlink++;
	iupdate(ip);

	// simplified FS: only root directory
	if ((dp = root_dir()) == 0) {
		ip->nlink--;
		iupdate(ip);
		iput(ip);
		return -1;
	}

	ivalid(dp);

	// create new directory entry
	if (dirlink(dp, new, ip->inum) < 0) {
		ip->nlink--;
		iupdate(ip);
		iupdate(dp);
		iput(dp);
		iput(ip);
		return -1;
	}

	iput(dp);
	iput(ip);

	return 0;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
	struct inode *dp, *ip;
	char path[MAXPATH];

	if(copyinstr(curr_proc()->pagetable, path, name, MAXPATH) < 0)
	{
		errorf("copyinstr");
		return -1;
	}

	debugf("sys_unlinkat path = %s", path);

	// only root directory
	if ((dp = root_dir()) == 0) {
		errorf("invalid rootdir");
		return -1;
	}

	ivalid(dp);

	// find inode
	if ((ip = namei(path)) == 0) {
		errorf("inode does not exist");
		iput(dp);
		return -1;
	}

	ivalid(ip);

	// remove entry (handles link_count + iput internally)
	if (dirunlink(dp, path) < 0) {
		errorf("dirunlink fail");
		iput(ip);
		iput(dp);
		return -1;
	}

	iput(ip);
	iput(dp);

	debugf("sys_unlink finished with no errors");
	return 0;
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
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
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
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
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
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall %d ret %d", id, ret);
}
