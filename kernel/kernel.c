#include <stdlib.h>
#include "kernel.h"
#include "list.h"

#include "enib_utils.h"

#ifndef NULL
#define NULL 0
#endif


/*****************************************************************************
 * Global variables
 *****************************************************************************/

static uint32_t id=1;
Task * tsk_running = NULL;	/* pointer to ready task list : the first
                                     node is the running task descriptor */
Task * tsk_prev = NULL;
Task * tsk_sleeping = NULL;	/* pointer to sleeping task list */

/*****************************************************************************
 * SVC dispatch
 *****************************************************************************/
/* sys_add
 *   test function
 */
int sys_add(int a, int b)
{
    return a+b;
}

int sys_sub(int a, int b)
{
    return a-b;
}

/* syscall_dispatch
 *   dispatch syscalls
 *   n      : syscall number
 *   args[] : array of the parameters (4 max)
 */
int32_t svc_dispatch(uint32_t n, uint32_t args[])
{
	int32_t result=-1;

    switch(n) {
      case 0:
          result = sys_add((int)args[0], (int)args[1]);
          break;
      case 1:
    	  result = malloc(args[0]);
    	  break;
      case 2:
    	  free(args[0]);
    	  result = 0;
    	  break;
      case 3:
    	  result = sys_os_start();
    	  break;
      case 4:
    	  result = sys_task_new(args[0], args[1]);
    	  break;
      case 5:
    	  result = sys_task_id();
    	  break;
      case 6:
    	  result = sys_task_wait(args[0]);
    	  break;
      case 7:
    	  result = sys_task_kill();
    	  break;
      case 8:
    	  result = sys_sem_new(args[0]);
    	  break;
      case 9:
    	  result = sys_sem_p(args[0]);
    	  break;
      case 10:
    	  result = sys_sem_v(args[0]);
    	  break;
    }
    return result;
}

void sys_switch_ctx()
{
	SCB->ICSR |= 1<<28; // set PendSV to pending
}
/*****************************************************************************
 * Round robin algorithm
 *****************************************************************************/
#define SYS_TICK  10	// system tick in ms

uint32_t sys_tick_cnt=0;

/* tick_cb
 *   system tick callback: task switching, ...
 */
void sys_tick_cb()
{
//	Task* prev = tsk_running;
	tsk_running->status = TASK_SLEEPING;
	tsk_prev = tsk_running;
	tsk_running = tsk_prev->next;
	tsk_running->status = TASK_RUNNING;
	sys_switch_ctx();

	//delay management

	int size = list_size(tsk_sleeping);
	for(int i = 0; i < size; ++i)
	{
		tsk_sleeping->delay -= SYS_TICK;
		if (tsk_sleeping->delay <= 0)//if timeout
		{
			tsk_sleeping->delay = 0;//reset delay

			Task* tskDelay;
			//get next sleeping task and store current to tskDelay
			tsk_sleeping = list_remove_head(tsk_sleeping,&tskDelay);
			tskDelay->status = TASK_READY;//set current task to ready
			tsk_running = list_insert_tail(tsk_running,tskDelay);//add it to running tasks
		}
		else//no timeout, just skip it
			tsk_sleeping = tsk_sleeping->next;

		sys_switch_ctx();
	}
}

void SysTick_Handler(void)
{
	sys_tick_cnt++;

	if (sys_tick_cnt == SYS_TICK) {
		sys_tick_cnt = 0;
		sys_tick_cb();
	}
}

/*****************************************************************************
 * General OS handling functions
 *****************************************************************************/

/* sys_os_start
 *   start the first created task
 */
int32_t sys_os_start()
{
	tsk_running->status = TASK_RUNNING;

	sys_switch_ctx();

    // Reset BASEPRI
    __set_BASEPRI(0);

	// Set systick reload value to generate 1ms interrupt
    SysTick_Config(SystemCoreClock / 1000U);
    return 0;
}

/*****************************************************************************
 * Task handling functions
 *****************************************************************************/
void task_kill();

/* sys_task_new
 *   create a new task :
 *   func      : task code to be run
 *   stacksize : task stack size
 *
 *   Stack frame:
 *      |    xPSR    | -> 17
 *      |     PC     | -> 16
 *      |     LR     | -> 15
 *      |     R12    |    ^ -> 14
 *      |     R3     |    ^ -> 13
 *      |     R2     |    | @++ -> 12
 *      |     R1     | -> 11
 *      |     R0     | -> 10
 *      +------------+
 *      |     R11    | -> 9
 *      |     R10    | -> 8
 *      |     R9     | -> 7
 *      |     R8     | -> 6
 *      |     R7     | -> 5
 *      |     R6     | -> 4
 *      |     R5     | -> 3
 *      |     R4     | -> 2
 *      +------------+
 *      | EXC_RETURN | -> 1
 *      |   CONTROL  | <- sp -> 0
 *      +------------+
 */
int32_t sys_task_new(TaskCode func, uint32_t stacksize)
{
	// get a stack with size multiple of 8 bytes
	uint32_t size = stacksize>96 ? 8*(((stacksize-1)/8)+1) : 96;
	
	Task* tsk = (Task*)malloc(sizeof(Task)+size);

	if(!tsk)//if malloc failed
		return -1;

	tsk->id = id++;
	tsk->status = TASK_READY;

//	Task* t2 = tsk+1;
	tsk->splim = (uint32_t*)(tsk+1);
	tsk->sp = tsk->splim+(size/4);

	tsk->sp -= 18;//reserve space for context

	tsk->sp[0] = (/*tsk->sp[0]*/0x0)|(0x1<<0);//CONTROL = unprivileged
	tsk->sp[1] = 0xFFFFFFFD;//EXC_RETURN = thread, psp
	tsk->sp[15] = (uint32_t)task_kill;//LR (calling context)
					//this will be called at the end of task function
	tsk->sp[16] = (uint32_t)func;//PC
	tsk->sp[17] = 1<<24;//xPSR = 1<<24

	//stack init

	tsk_running = list_insert_tail(tsk_running, tsk);
	if(tsk_running == NULL)
	{
		if(tsk != NULL)
		{
			free(tsk);//free allocated memory
		}
		return -1;
	}

	return tsk->id;
}

/* sys_task_kill
 *   kill oneself
 */
int32_t sys_task_kill()
{
	Task *tskToKill;
	tsk_running = list_remove_head(tsk_running,&tskToKill);
	tsk_running->status = TASK_RUNNING;
	sys_switch_ctx();
	free(tskToKill);

	return 0;
}

/* sys_task_id
 *   returns id of task
 */
int32_t sys_task_id()
{
    return tsk_running->id;
}


/* sys_task_yield
 *   run scheduler to switch to another task
 */
int32_t sys_task_yield()
{

    return -1;
}

/* task_wait
 *   suspend the current task until timeout
 */
int32_t sys_task_wait(uint32_t ms)
{
	tsk_running = list_remove_head(tsk_running, &tsk_prev);
	//error checking might become costly considering how many time this function
	//will be called
//	if(!tsk_running)
//		return -1;
	tsk_sleeping = list_insert_tail(tsk_sleeping, tsk_prev);
//	if(!tsk_sleeping)
//		return -1;

	tsk_prev->delay = ms;
	tsk_prev->status = TASK_WAITING;
	tsk_running->status = TASK_RUNNING;

	sys_switch_ctx();

	return 0;
}


/*****************************************************************************
 * Semaphore handling functions
 *****************************************************************************/

/* sys_sem_new
 *   create a semaphore
 *   init    : initial value
 */
Semaphore * sys_sem_new(int32_t init)
{
	Semaphore* sem = (Semaphore*)malloc(sizeof(Semaphore));
	sem->count = init;
	sem->waiting = NULL;

    return sem;
}

/* sys_sem_p
 *   take a token
 */
int32_t sys_sem_p(Semaphore * sem)
{
	--sem->count;
	if(sem->count < 0)
	{
		Task* tsk;

		tsk_running = list_remove_head(tsk_running, &tsk);

		sem->waiting = list_insert_tail(sem->waiting, tsk);
		sem->waiting->status = TASK_WAITING;
		tsk_running->status = TASK_RUNNING;

		tsk_prev = tsk;
		sys_switch_ctx();
	}

	return sem->count;
}

/* sys_sem_v
 *   release a token
 */
int32_t sys_sem_v(Semaphore * sem)
{
	++sem->count;
	if(sem->waiting != NULL)
	{
		Task* task;

		sem->waiting = list_remove_head(sem->waiting, &task);

		tsk_prev = tsk_running;

		tsk_running = list_insert_head(tsk_running, task);
		task->status = TASK_RUNNING;
		tsk_prev->status = TASK_READY;

		sys_switch_ctx();
	}

	return sem->count;
}
