#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc,char *argv[])
{
	struct sched_param parameter;

    pid_t pid;
	int policy, prio;
	int pre_sched, cur_sched;
	
	printf("Please input the Choice of Scheduling algorithms (0-NORMAL,1-FIFO,2-RR,6-WRR): ");
	scanf("%d", &policy);

	printf("Current scheduling algorithm is ");
	switch (policy) {
	    case 0: printf("SCHED_NORMAL\n");break;
	    case 1: printf("SCHED_FIFO\n"); break;
	    case 2: printf("SCHED_RR\n"); break;
	    case 6: printf("SCHED_WRR\n"); 
	}
	
	printf("Please input the id of the testprocess : ");
	scanf("%d", &pid);

	/*only for rr*/
	printf("Set Process's priority(1-99): ");
	scanf("%d", &prio);
	printf("Current scheduler priority is %d\n", prio);

	pre_scheduler = sched_getscheduler(pid);
	printf("pre scheduler : ");
	switch (pre_scheduler) {
	case 0: printf("SCHED_NORMAL\n"); break;
	case 1: printf("SCHED_FIFO\n"); break;
	case 2: printf("SCHED_RR\n"); break;
	case 6: printf("SCHED_WRR\n");
	}
	
	param.sched_priority = prio;
	sched_setscheduler(pid, policy, &parameter);

	printf("cur scheduler : ");
	cur_scheduler = sched_getscheduler(pid);
	switch (cur_scheduler) {
	case 0: printf("SCHED_NORMAL\n"); break;
	case 1: printf("SCHED_FIFO\n"); break;
	case 2: printf("SCHED_RR\n"); break;
	case 6: printf("SCHED_WRR\n");
	}

	return 0;
}
