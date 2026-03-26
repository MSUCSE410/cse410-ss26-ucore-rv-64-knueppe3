#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "log.h"
#include "riscv.h"
#include "trap.h"
#include "timer.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid()
{
	return curr_proc()->pid;
}

struct proc *curr_proc()
{
	return current_proc;
}

// initialize the proc table at boot time.
void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->trapframe = (struct trapframe *)trapframe[p - pool];
		p->taskinfo.status = UnInit;
		p->taskinfo.time = 0;
	}
	idle.kstack = (uint64)boot_stack_top;
	idle.pid = IDLE_PID;
	current_proc = &idle;
	//init_queue(&task_queue);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

struct proc *fetch_task()
{
	int index = pop_queue(&task_queue);
	if (index < 0) {
		debugf("No task to fetch\n");
		return NULL;
	}
	debugf("fetch task %d(pid=%d) to task queue\n", index, pool[index].pid);
	return pool + index;
}

void add_task(struct proc *p)
{
	//push_queue(&task_queue, p - pool);
	debugf("add task %d(pid=%d) to task queue\n", p - pool, p->pid);
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;
		}
	}
	return 0;

found:
	// init proc
	p->pid = allocpid();
	p->state = USED;
	p->taskinfo.status = Ready;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate((uint64)p->trapframe);
	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;

	uint64 sec = get_cycle() / CPU_FREQ;
	uint64 usec = (get_cycle() % CPU_FREQ) * 1000000 / CPU_FREQ;

	p->taskinfo.time = (sec * 1000 + usec / 1000);

	for (int i = 0; i < MAX_SYSCALL_NUM; ++i)
	{
		p->taskinfo.syscall_time[i] = 0;
	}

	// priority stuff
	p->stride = 0;
	p->priority = 16;

	return p;
}

// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler()
{
	struct proc *p = NULL;
	struct proc *np;
	for (;;) {
		/*int has_proc = 0;
		for (p = pool; p < &pool[NPROC]; p++) {
			if (p->state == RUNNABLE) {
				has_proc = 1;
				tracef("swtich to proc %d", p - pool);
				p->state = RUNNING;
				current_proc = p;
				swtch(&idle.context, &p->context);
			}
		}
		if(has_proc == 0) {
			panic("all app are over!\n");
		}*/

		for (np = pool; np < &pool[NPROC]; ++np)
		{
			if ((p == NULL || p->state == ZOMBIE) && np->state == RUNNABLE)
			{
				p = np;
				debugf("p == null, np stride=%d, p stride=%d", np->stride, p->stride);
			}
			else if (np->state == RUNNABLE && np->stride < p->stride)
			{
				debugf("p != null, np stride=%d, p stride=%d", np->stride, p->stride);
				p = np;
			}	
		}
		debugf("switch to process %d", p->pid);
		// shell process is done
		if (pool->state == UNUSED) {
			panic("all app are over!\n");
		}
		p->state = RUNNING;
		p->taskinfo.status = Running;
		int pass = BIG_STRIDE / p->priority;
		p->stride += pass;
		current_proc = p;
		swtch(&idle.context, &p->context);
	}
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched()
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield()
{
	current_proc->state = RUNNABLE;
	current_proc->taskinfo.status = Ready;
	//add_task(current_proc);
	sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, uint64 max_page)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p)
{
	if (p->pagetable)
		freepagetable(p->pagetable, p->max_page);
	p->pagetable = 0;
	p->state = UNUSED;
}

int fork()
{
	debugf("process forked");
	struct proc *np;
	struct proc *p = curr_proc();
	// Allocate process.
	if ((np = allocproc()) == 0) {
		panic("allocproc\n");
	}
	// Copy user memory from parent to child.
	if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
		panic("uvmcopy\n");
	}
	np->max_page = p->max_page;
	// copy saved user registers.
	*(np->trapframe) = *(p->trapframe);
	// Cause fork to return 0 in the child.
	np->trapframe->a0 = 0;
	np->parent = p;
	np->state = RUNNABLE;
	//add_task(np);
	return np->pid;
}

int exec(char *name)
{
	debugf("name [%s]", name);
	int id = get_id_by_name(name);
	if (id < 0)
		return -1;
	struct proc *p = curr_proc();
	uvmunmap(p->pagetable, 0, p->max_page, 1);
	p->max_page = 0;
	loader(id, p);
	return 0;
}

int wait(int pid, int *code)
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();

	for (;;) {
		// Scan through table looking for exited children.
		havekids = 0;
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != UNUSED && np->parent == p && (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				//infof("havekids = 1, zombie=%d", np->state == ZOMBIE);
				if (np->state == ZOMBIE) {
					// Found one.
					np->state = UNUSED;
					pid = np->pid;
					*code = np->exit_code;
					return pid;
				}
			}	
		}
		//infof("out of first loop");
		if (!havekids) {
			return -1;
		}
		p->state = RUNNABLE;
		//add_task(p);
		sched();
	}
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	p->exit_code = code;
	p->taskinfo.status = Exited;
	debugf("proc %d exit with %d\n", p->pid, code);
	freeproc(p);
	if (p->parent != NULL) {
		// Parent should `wait`
		infof("made zombie with process %d", p->pid);
		p->state = ZOMBIE;
	}
	// Set the `parent` of all children to NULL
	struct proc *np;
	for (np = pool; np < &pool[NPROC]; np++) {
		if (np->parent == p) {
			np->parent = NULL;
		}
	}
	sched();
}

// Spawns a new process
int spawn(char* filename)
{
	struct proc* new_process;
	struct proc* curr_process = curr_proc();

	int id = get_id_by_name(filename);
	if (id < 0)
	{
		errorf("invalid name %s, id %d", filename, id);
		return -1;
	}	
	
	// alloc proc
	if ((new_process = allocproc()) == 0)
	{
		errorf("spawn allocproc\n");
		return -1;
	}

	// transfer more
	*(new_process->trapframe) = *(curr_process->trapframe);
	new_process->trapframe->a0 = 0;
	new_process->parent = curr_process;
	new_process->state = RUNNABLE;
 
	if (new_process->pagetable == 0)
	{
		panic("spawn pagetable");
	}

	debugf("spawned pid=%d", new_process->pid);
	//add_task(new_process);
	loader(id, new_process);
	return new_process->pid;
}
