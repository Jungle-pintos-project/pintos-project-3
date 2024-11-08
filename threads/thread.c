#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
struct list sleep_list;
static struct list wait_list;

/* Idle thread. */
// 유휴 쓰레드
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

bool cmp_thread_priority (const struct list_elem *a, const struct list_elem *b, void *aux) {
	return list_entry(a, struct thread, elem)->priority > list_entry(b, struct thread, elem)->priority;
}

bool cmp_thread_donations_priority (const struct list_elem *a, const struct list_elem *b, void *aux) {
	return list_entry(a, struct thread, d_elem)->priority > list_entry(b, struct thread, d_elem)->priority;
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */

/**
 * 쓰레드 시스템을 초기화
 * run queue 와 tid lock, sleep list 초기화
 * 호출 후 쓰레드를 생성하기 전 page allocator 를 초기화해야함
 */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	// run queue
	list_init (&ready_list);
	// 쓰레드 종료 후 자원 정리용 
	list_init (&destruction_req);

	// sleep list 
	list_init (&sleep_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */

/**
 * 인터럽트 활성화과 함께 선점형 쓰레드 스케줄링 시작 유휴 쓰레드 생성
 * -> 유휴 쓰레드는 사용가능한 상태지만 작업은 없는 상태의 쓰레드를 의미
 */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */

/**
 * 타이머 인터럽트 핸들러에 의해 매 타이머 tick 마다 호출되는 함수
 * 레드의 실행 시간을 추적하고, 필요할 경우 선점 preemption 유도해 스케줄링 결정을 내리게 함
 * 이 기능은 외부 인터럽트 컨텍스트에서 실행
 */
void
thread_tick (void) {
	// 현재 실행중인 쓰레드
	struct thread *t = thread_current ();

	/* Update statistics. */
	// CPU 가 아무것도 하지 않는 상태라면 작업을 기다리는 상태,
	// 즉, 현재 실행중인 쓰레드를 가리키는 current 가 idle 쓰레드 라면 idle tick 증가
	// 그 외에는 현재 쓰레드가 커널 쓰레드라는 의미임
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */

/**
 * 쓰레드를 생성해서 run queue 에 넣는데 
 * 새로운 쓰레드의 우선순위가 더 높으면 새 쓰레드를 선택해 CPU 제공
 */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */

	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;
	
	/* Add to run queue. */
	thread_unblock (t);

	/**
	 * compare the priorities of the currently running thread
	 * and the newly inserted one. Yield the CPU if the newly
	 * arriving thread has higher priority
	 */
	
	// 새로 들어온 쓰레드가 실행중인 쓰레드보다 우선순위가 높으면
	// if (aux) {
	// 	enum intr_level old_level;
	// 	struct lock *lock = aux;
	// 	if (lock->holder == NULL) {
	// 		lock->holder = t;
	// 	} else {
	// 		old_level = intr_disable ();
	// 		list_insert_ordered(&lock->semaphore.waiters, &t->elem, cmp_thread_priority, NULL);
	// 		intr_set_level (old_level);
	// 	}
	// }
	// 	struct thread *cur = thread_current();

	// 	if(lock->holder != NULL) {
	// 		list_insert_ordered(&lock->holder->donations, &cur->d_elem, cmp_thread_donations_priority, NULL);
	// 		cur->wait_on_lock = lock;
	// 		while(cur->wait_on_lock == NULL) {
	// 			cur->wait_on_lock->holder->priority = cur->priority;
	// 			cur = cur->wait_on_lock->holder;
	// 		}
	// 	}

	preemption();
	// if(!list_empty(&ready_list) && thread_current()->priority < t->priority) {
	// 	// 새 쓰레드 선택 ㄱㄱ
	// 	thread_yield();
	// }

	// if (!list_empty(&cur->donations)) {
	// 	list_sort(&cur->donations, cmp_thread_donations_priority, NULL);
	// 	struct thread *t = list_entry(list_front(&cur->donations), struct thread, d_elem);
	// 	if (cur->priority < t->priority) {
	// 		cur->priority = t->priority;
	// 	}
	// }

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */

/**
 * 현재 실행 중인 쓰레드를 블록 상태 (sleep) 로 변경
 * 해당 쓰레드는 다시 실행되지 않으며 thread_unblock() 깨워질 때까지 실행 안댐
 */
void
thread_block (void) {
	ASSERT (!intr_context ());
	// 인터럽트가 꺼진 상태에서 실행해야대!!!
	ASSERT (intr_get_level () == INTR_OFF);
	// curr 쓰레드 상태를 BLOCKED (sleep)
	thread_current ()->status = THREAD_BLOCKED;
	// curr 쓰레드를 재워났으니 다음 실행할거 찾음
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */

/**
 * 블록 상태 (SLEEP) 의 쓰레드를 실행가능 상태 (READY) 로 전환
 * 해당 쓰레드를 ready list 에 추가
 */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);

	// 기존에는 그냥 run queue 뒤에 넣음
	// list_push_back (&ready_list, &t->elem);
	// 우선순위가 높은 애가 먼저 나오게 정렬 후 넣음
	list_insert_ordered(&ready_list, &t->elem, cmp_thread_priority, NULL);
	if (t->priority < thread_current()->priority) {
		thread_yield();
	}
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */

// curr thread 반환
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */

/** 
 * 실행할 새 쓰레드를 선택하는 스케쥴러에게 CPU를 제공
 */
void
thread_yield (void) {
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT (!intr_context ());
	// 인터럽트 이전 상태 저장 및 비활성화
	old_level = intr_disable (); 

	// curr 쓰레드 가 idle 쓰레드가 아니라면 
	// 그니까 현재 실행중이었던 쓰레드가 있으면 다시 넣어줘야겠죠?
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem, cmp_thread_priority, NULL);
	
	// 쓰레드를 READY 상태로 설정하고 
	do_schedule (THREAD_READY);
	// 이전에 인터럽트가 걸려있었으면 다시 걸고 아니면 안걸겠죠?
	intr_set_level (old_level);
}

bool cmp_thread_tick (const struct list_elem *a, const struct list_elem *b, void *aux) {
	return list_entry(a, struct thread, elem)->wakeup_tick < list_entry(b, struct thread, elem)->wakeup_tick;
}

/**
 * 현재 실행 중인 스레드를 일정 시간 동안 재우기 위해 블록 상태로 변경하는 함수
 * 현재 스레드가 깨어날 시점 (tick 값) 을 기록하고
 * 쓰레드를 BLOCKED 상태로 전환해서 다른 쓰레드가 실행될 수 있도록 함
 * tick 시간이 지나면 깨어나도록 예약
 */
void
thread_sleep (int64_t ticks) {
	/** 
	 * 만약 curr thread 이 idle thread 가 아니라면 caller thread 를 BLOCKED 상태로 변경
	 * wakeup 을 위해 local tick 을 추가하고
	 * 필요한 경우 global tick 을 업데이트하고, schedule() 을 호출한다.
	 * 쓰레드를 조작할때에는 인터럽트를 비활성화해라
	 * 
	 * if the current thread is not idle thread, 
	 * change the state of the caller thread to BLOCKED
	 * store the local tick to wake up, 
	 * update the global tick if necessary, and call schedule().
	 * 
	 * when you manipulate thread list, disable interrupt!
	 */
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	// disable interrupt
	old_level = intr_disable (); 

	// store the local tick
	// 현재 스레드가 깨어나야 할 tick 값을 저장
	curr->wakeup_tick = ticks;
	
	list_insert_ordered(&sleep_list, &curr->elem, cmp_thread_tick, NULL);
	// list_push_back(&sleep_list, &curr->elem);
	if (curr != idle_thread) {
		thread_block();
	}
	intr_set_level(old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */

/**
 * 현재 실행 중인 쓰레드의 우선순위를 변경
 */
void
thread_set_priority (int new_priority) {
	struct thread *cur = thread_current();
	cur->origin_priority = new_priority;
	cur->priority = cur->origin_priority;

	if (!list_empty(&cur->donations)) {
		list_sort(&cur->donations, cmp_thread_donations_priority, NULL);
		struct thread *t = list_entry(list_front(&cur->donations), struct thread, d_elem);
		if (cur->priority < t->priority) {
			cur->priority = t->priority;
		}
	}
	preemption();
}

void
preemption (void) {
	if(list_empty(&ready_list)) {
		return;
	}

	// 실행중인 쓰레드의 우선순위가 바뀌면 자연스럽게 ready list 의 쓰레드가 우선순위가 더 높아질 수 잇음
	if(thread_current()->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority) {
		thread_yield();
	}
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	t->origin_priority = priority;
	t->wait_on_lock = NULL;
	list_init(&t->donations);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */

// 현재 쓰레드의 상태를 변경하고 새로운 쓰레드로 전환 Context Switch
static void
do_schedule(int status) {
	// 인터럽트 꺼놔야하고
	ASSERT (intr_get_level () == INTR_OFF);
	// 쓰레드가 실행 상태여야 함
	ASSERT (thread_current()->status == THREAD_RUNNING);
	// destruction_req 삭졔되어야 할 쓰레드가 있으면
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		// 삭제 ㄱㄱ
		palloc_free_page(victim);
	}

	// curr 쓰레드 상태를 바꾸고
	thread_current ()->status = status;
	// 스케줄러를 호출해 새 쓰레드로 전환
	schedule ();
}

// 현재 실행 중인 쓰레드를 다음에 실행할 쓰레드로 교체 Scheduler
static void
schedule (void) {
	// 햔재 실행중인 쓰레드
	struct thread *curr = running_thread ();
	// 다음에 실행될 쓰레드
	struct thread *next = next_thread_to_run ();

	// 인터럽트 꺼져야 댐
	ASSERT (intr_get_level () == INTR_OFF);
	// 현재 실행상태면 안댐
	ASSERT (curr->status != THREAD_RUNNING);
	// 다음 실행할 쓰레드가 있어야 댐
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
