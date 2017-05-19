#include <pthread.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netpbm/ppm.h>

#define POOL_SIZE 4
#define PRIORITY_LEVEL POOL_SIZE
#define MAXSLOTS 20
#define DEFAULT_M MAXSLOTS

#ifndef BOOL
#define BOOL	unsigned char
#undef TRUE
#undef FALSE
#define TRUE	1
#define FALSE	0
#endif

#define DEFAULT_TIME 1

#ifdef MY_MONITOR
#define Thread_Mutex_Lock(a)		my_mutex_lock(a)
#define Thread_Mutex_Unlock(a)		my_mutex_unlock(a)
#define Thread_Cond_Wait(a, b)		my_cond_wait(a, b)
#define Thread_Cond_Signal(a)		my_cond_signal(a)
#define Thread_Cond_Broadcast(a)	my_cond_broadcast(a)
#define Thread_Cond_Init(a, b)		//TODO:init
#define Thread_Cond_t				//TODO:type
#define Thread_Mutex_t				//TODO:type
#define Cond_Initializer			//TODO:value
#define Mutex_Initializer			//TODO:value
#else
#define Thread_Mutex_Lock(a)		pthread_mutex_lock(a)
#define Thread_Mutex_Unlock(a)		pthread_mutex_unlock(a)
#define Thread_Cond_Wait(a, b)		pthread_cond_wait(a, b)
#define Thread_Cond_Signal(a)		pthread_cond_signal(a)
#define Thread_Cond_Broadcast(a)	pthread_cond_broadcast(a)
#define Thread_Cond_Init(a, b)		pthread_cond_init(a, b)
#define Thread_Cond_t				pthread_cond_t
#define Thread_Mutex_t				pthread_mutex_t
#define Cond_Initializer			PTHREAD_COND_INITIALIZER
#define Mutex_Initializer			PTHREAD_MUTEX_INITIALIZER
#endif

#define MAX_NAME 20
#define MAX_REQUEST MAX_NAME
#define MAX_MOVIE MAX_NAME

#define VIDEO

struct my_work{
	int sd;
};

struct thread_pool{
	pthread_t threads[POOL_SIZE];
	Thread_Cond_t starts[POOL_SIZE];
	Thread_Mutex_t plock, full_lock;
	BOOL busys[POOL_SIZE];
	struct my_work *workload;
	BOOL full;
	BOOL shutdown;
	BOOL take;
}my_pool = {
	.full = FALSE,
	.shutdown = FALSE,
	.plock = Mutex_Initializer,
	.full_lock = Mutex_Initializer,
	.take = TRUE
};


struct client_id{
	int pid;
	char name[MAX_NAME];	/* should be null-terminated */
};

struct package{
	struct client_id ID;
	int prior;
	char request[MAX_REQUEST];	/* should be null-terminated */
	union{
		BOOL repeat;
		char movie[MAX_MOVIE];	/* should be null-terminated */
		int num;
	}arg;
};

struct item{
	pthread_t pid;
	int sd;
	struct package client;
	int finish;
#ifdef VIDEO
	unsigned char buffer[160 * 120 * 3];
#endif
};

struct buffer{
	struct item data[MAXSLOTS];
	Thread_Mutex_t m_wait;
	Thread_Cond_t size;
	BOOL full, set_disp;
	int m;
	int front, end;
	Thread_Cond_t full_wait;	/* should be broadcasted */
}my_buffer = {
	.m_wait = Mutex_Initializer,
	.size = Cond_Initializer,
	.full_wait = Cond_Initializer,
	.full = FALSE,
	.m = DEFAULT_M,
	.front = 0,
	.end = 0,
	.set_disp = FALSE
};

pthread_t Thread_disp;
pthread_t Thread_man;
BOOL Thr_man_run = FALSE;

struct DynManage{
	GSList *threads;
	GSList *starts;
	GSList *busys;
	Thread_Mutex_t b_lock, w_lock, t_lock;
	struct my_work *workload;
	BOOL take;
	int busy_count;
	BOOL shutdown, idle; /* busy_count, idle, clear_sig: for threads clearance */
	Thread_Cond_t clear_sig;
	int time;
}my_manager = {
	.threads = NULL,
	.starts = NULL,
	.busys = NULL,
	.b_lock = Mutex_Initializer,
	.w_lock = Mutex_Initializer,
	.t_lock = Mutex_Initializer,
	.busy_count = 0,
	.shutdown = FALSE,
	.idle = FALSE,
	.clear_sig = Cond_Initializer,
	.time = DEFAULT_TIME,
	.take = TRUE
};

/* clear dynamically created threads */
void* clear(void *data){
	while(my_pool.shutdown != TRUE){
		Thread_Mutex_Lock(&my_manager.b_lock);
		Thread_Cond_Wait(&my_manager.clear_sig, &my_manager.b_lock);
		Thread_Mutex_Unlock(&my_manager.b_lock);

		Thr_man_run = TRUE;
		my_manager.idle = TRUE;
		/* T */
		sleep(my_manager.time);

		Thread_Mutex_Lock(&my_manager.t_lock);
		
		/* idle for more than T seconds */
		if(my_manager.idle == TRUE){
			/* clear threads */
			my_manager.shutdown = TRUE;
			int num = g_slist_length(my_manager.threads);
			int i;
			
			Thread_Mutex_Lock(&my_manager.w_lock);

			GSList *temp;
			for(i = 0; i < num; i++){
				Thread_Cond_Signal((Thread_Cond_t *)(g_slist_nth(my_manager.starts, i)->data));
				temp = g_slist_nth(my_manager.threads, i);
				free((pthread_t *)temp->data);
			}
			g_slist_free(my_manager.threads);
			my_manager.threads = NULL;

#ifdef DEBUG
			/* test */
			printf("clear %d\n", num);
#endif

			Thread_Mutex_Unlock(&my_manager.w_lock);

			my_manager.idle = FALSE;
		}

		Thread_Mutex_Unlock(&my_manager.t_lock);
		
		Thr_man_run = FALSE;
	}
	return NULL;
}

void produce(struct item *data)
{
	Thread_Mutex_Lock(&my_buffer.m_wait);
	if(my_buffer.full == TRUE)
		do{
			Thread_Cond_Wait(&my_buffer.full_wait, &my_buffer.m_wait);
			/* test */
			printf("%d\n", my_buffer.full);

		}while(my_buffer.full == TRUE);

	my_buffer.data[my_buffer.front] = *data;
	my_buffer.front = (my_buffer.front + 1) % MAXSLOTS;
	if(my_buffer.front >= my_buffer.end &&
		my_buffer.front - my_buffer.end >= my_buffer.m){
		if(my_buffer.set_disp == FALSE){
			my_buffer.set_disp = TRUE;
			Thread_Cond_Signal(&my_buffer.size);

			/* test */
//			printf("front:%d, end:%d\n", my_buffer.front, my_buffer.end);
		}
	}
	else if(my_buffer.front < my_buffer.end &&
		my_buffer.end - my_buffer.front <= MAXSLOTS - my_buffer.m){
		if(my_buffer.set_disp == FALSE){
			my_buffer.set_disp = TRUE;
			Thread_Cond_Signal(&my_buffer.size);

			/* test */
//			printf("front:%d, end:%d\n", my_buffer.front, my_buffer.end);
		}
	}
	else{
		/* last frame, call dispatch anyway */
		if(data->client.ID.pid == 100){
			if(my_buffer.set_disp == FALSE){
				my_buffer.set_disp = TRUE;
				Thread_Cond_Signal(&my_buffer.size);
			}
		}
	}

	if(my_buffer.front == my_buffer.end)
		my_buffer.full = TRUE;

	Thread_Mutex_Unlock(&my_buffer.m_wait);
}


void* dispatch(void *data)
{
	while(my_pool.shutdown != TRUE){
		Thread_Mutex_Lock(&my_buffer.m_wait);

		Thread_Cond_Wait(&my_buffer.size, &my_buffer.m_wait);

		/* reorder buffer */
		int i, j, prev;
		struct item temp;
		for(i = (my_buffer.end + 1) % MAXSLOTS; i != my_buffer.front;
			i = (i + 1) % MAXSLOTS){
			prev = my_buffer.end;
			for(j = (my_buffer.end + 1) % MAXSLOTS; j != my_buffer.front;
				j = (j + 1) % MAXSLOTS){
				if(my_buffer.data[prev].client.prior <
					my_buffer.data[j].client.prior){
					temp = my_buffer.data[prev];
					my_buffer.data[prev] = my_buffer.data[j];
					my_buffer.data[j] = temp;
				}
				prev = j;
			}
		}

		/* take out all items and send them */
		for(i = my_buffer.end; i != my_buffer.front; i = (i + 1) % MAXSLOTS){
#ifndef VIDEO
			write(my_buffer.data[my_buffer.end].sd, &my_buffer.data[my_buffer.end],
				sizeof(struct item));
#else
			write(my_buffer.data[my_buffer.end].sd, my_buffer.data[my_buffer.end].buffer, 120 * 160 * 3);
#endif
#ifdef DEBUG
			printf("pid:%u, frame:%d, priority:%d\n", my_buffer.data[my_buffer.end].pid, 
				my_buffer.data[my_buffer.end].client.ID.pid, my_buffer.data[my_buffer.end].client.prior);
#endif

			if(my_buffer.data[my_buffer.end].client.ID.pid == 100) close(my_buffer.data[my_buffer.end].sd);
			my_buffer.end = (my_buffer.end + 1) % MAXSLOTS;
		}

		if(my_buffer.full == TRUE){
			/* test */
//			printf("%d, %d, %d\n", my_buffer.full, my_buffer.front, my_buffer.end);

			Thread_Cond_Broadcast(&my_buffer.full_wait);
		}
		my_buffer.full = FALSE;

		my_buffer.set_disp = FALSE;

		/* test */
//		printf("front:%d, end:%d\n", my_buffer.front, my_buffer.end);

		Thread_Mutex_Unlock(&my_buffer.m_wait);
	}
	return NULL;
}


/* for thread pool threads */
void* do_work(void *data)
{
	int index = *(int *)data;
	free((int *)data);
	struct my_work *cur;
	BOOL first = TRUE;

	while(1){
		if(first == FALSE){
			my_pool.busys[index] = FALSE;
			Thread_Mutex_Lock(&my_pool.full_lock);
			my_pool.full = FALSE;
			Thread_Mutex_Unlock(&my_pool.full_lock);
		}

		Thread_Mutex_Lock(&my_pool.plock);

		/* wait for request */
		Thread_Cond_Wait(&my_pool.starts[index], &my_pool.plock);

		if(my_pool.shutdown == TRUE){
			Thread_Mutex_Unlock(&my_pool.full_lock);
			pthread_exit(NULL);
		}

		struct item load;

		cur = my_pool.workload;
		my_pool.take = TRUE;

		Thread_Mutex_Unlock(&my_pool.plock);

		/* receive package, blocking */
		read(cur->sd, &load.client, sizeof(struct package));

		load.pid = pthread_self();
		load.sd = cur->sd;

		/* test */
		printf("pid:%d, sd:%d, id:%d, host:%s, priority:%d, request:%s\n",
			load.pid, load.sd, load.client.ID.pid,
			load.client.ID.name, load.client.prior, load.client.request);

		int counter = 1;
		char path[50];
		int x, y;
		while(1){
				//read(cur->sd, &load.client, sizeof(struct package));
				//if(errno == EAGAIN || errno == EWOULDBLOCK) ;
				//else break;

			if(counter > 100) break;

#ifdef VIDEO
			sprintf(path, "images/sw%d.ppm", counter);
			FILE *p = fopen(path, "r");
			pixel **pixarray;
			int cols, rows;
			pixval maxval;
			pixarray = ppm_readppm (p, &cols, &rows, &maxval);
			fclose(p);

			for(y = 0; y < 120; y++)
				for(x = 0; x < 160; x++){
					load.buffer[(y * 160 + x) * 3 + 0] = PPM_GETR(pixarray[120-y-1][x]);
					load.buffer[(y * 160 + x) * 3 + 1] = PPM_GETG(pixarray[120-y-1][x]);
					load.buffer[(y * 160 + x) * 3 + 2] = PPM_GETB(pixarray[120-y-1][x]);
				}

			ppm_freearray (pixarray, rows);
#endif

			load.client.ID.pid = counter;
			if(counter == 100) load.finish = 1;
			else load.finish = 0;

				/* add to buffer */
			produce(&load);

				/* another frame */
			counter++;
		}
			//if(strcmp(load.client.request, "stop_movie") == 0){
				/* stop */
			//}
			//else if(strcmp(load.client.request, "seek_movie") == 0){
				//if(load.client.arg.num < 1) ;
				//else{
				//	counter = load.client.arg.num;
				//	goto AAA;
				//}
			//}
			//else{
				/* start_movie not supported for second time, set up another connection to start new movie */
				/* just keep playing */
				//if(counter > 100) ;
				//else goto AAA;
			//}
#ifdef DEBUG
		printf("Before Free-%u:%X\n", pthread_self(), cur);
#endif
		free(cur);
		
		if(first == TRUE) first = FALSE;
	}
	return NULL;
}

/* for dynamically created threads */
void* do_work_2(void *data)
{
	int index = *(int *)data;
	free((int *)data);
	struct my_work *cur;
	BOOL first = TRUE;
	GSList *temp = g_slist_nth(my_manager.starts, index);
	GSList *temp2 = g_slist_nth(my_manager.busys, index);

	while(1){
		Thread_Mutex_Lock(&my_manager.w_lock);

		if(first == FALSE){
			*(BOOL *)g_slist_nth_data(my_manager.busys, index) = FALSE;

			Thread_Mutex_Lock(&my_manager.b_lock);
			my_manager.busy_count--;
			if(my_manager.busy_count == 0){
				if(Thr_man_run == FALSE){
					my_manager.idle = TRUE;
					Thread_Cond_Signal(&my_manager.clear_sig);
				}
			}
			Thread_Mutex_Unlock(&my_manager.b_lock);

			/* wait for request */
			Thread_Cond_t *cond_temp = (Thread_Cond_t *)temp->data;
			Thread_Cond_Wait(cond_temp, &my_manager.w_lock);
		}

		if(my_manager.shutdown == TRUE){
			free((Thread_Cond_t *)temp->data);
			my_manager.starts = g_slist_delete_link(my_manager.starts, temp);
			free((BOOL *)temp2->data);
			my_manager.busys = g_slist_delete_link(my_manager.busys, temp2);
			Thread_Mutex_Unlock(&my_pool.plock);
			pthread_exit(NULL);
		}

		struct item load;

		cur = my_manager.workload;
		my_manager.take = TRUE;

		Thread_Mutex_Unlock(&my_manager.w_lock);

		/* receive package, blocking */
		read(cur->sd, &load.client, sizeof(struct package));

		load.pid = pthread_self();
		load.sd = cur->sd;

		/* test */
		printf("pid:%d, sd:%d, id:%d, host:%s, priority:%d, request:%s\n",
			load.pid, load.sd, load.client.ID.pid,
			load.client.ID.name, load.client.prior, load.client.request);


		int counter = 1;
		char path[50];
		int x, y;
		while(1){
				//read(cur->sd, &load.client, sizeof(struct package));
				//if(errno == EWOULDBLOCK || errno == EAGAIN) ;
				//else break;

			if(counter > 100) break;

#ifdef VIDEO
			sprintf(path, "images/sw%d.ppm", counter);
			FILE *p = fopen(path, "r");
			pixel **pixarray;
			int cols, rows;
			pixval maxval;
			pixarray = ppm_readppm (p, &cols, &rows, &maxval);
			fclose(p);

				/* test */
			load.client.ID.pid = counter;

			for(y = 0; y < 120; y++)
				for(x = 0; x < 160; x++){
					load.buffer[(y * 160 + x) * 3 + 0] = PPM_GETR(pixarray[120-y-1][x]);
					load.buffer[(y * 160 + x) * 3 + 1] = PPM_GETG(pixarray[120-y-1][x]);
					load.buffer[(y * 160 + x) * 3 + 2] = PPM_GETB(pixarray[120-y-1][x]);
				}

			ppm_freearray (pixarray, rows);
#endif
			load.client.ID.pid = counter;
			if(counter == 100) load.finish = 1;
			else load.finish = 0;

				/* add to buffer */
			produce(&load);

				/* another frame */
			counter++;
		}
			//if(strcmp(load.client.request, "stop_movie") == 0){
				/* stop */
			//}
			//else if(strcmp(load.client.request, "seek_movie") == 0){
				//if(load.client.arg.num < 1) ;
				//else{
				//	counter = load.client.arg.num;
				//	goto AAA;
				//}
			//}
			//else{
				/* start_movie not supported for second time, set up another connection to start new movie */
				/* just keep playing */
				//if(counter > 100) ;
				//else goto AAA;
			//}
#ifdef DEBUG
		printf("Before Free-%u:%X\n", pthread_self(), cur);
#endif
		free(cur);

		if(first == TRUE) first = FALSE;
	}
	return NULL;
}

void servConn(int port)
{
	int sd, new_sd;
	struct sockaddr_in name, cli_name;
	int sock_opt_val = 1;
	int cli_len;

	if ((sd = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("(servConn): socket() error");
		exit (-1);
	}

	if (setsockopt (sd, SOL_SOCKET, SO_REUSEADDR, (char *) &sock_opt_val,
				sizeof(sock_opt_val)) < 0) {
		perror ("(servConn): Failed to set SO_REUSEADDR on INET socket");
		exit (-1);
	}

	name.sin_family = AF_INET;
	name.sin_port = htons (port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind (sd, (struct sockaddr *)&name, sizeof(name)) < 0) {
		perror ("(servConn): bind() error");
		exit (-1);
	}

	listen (sd, 5);

	for (;;){
		cli_len = sizeof (cli_name);
		new_sd = accept (sd, (struct sockaddr *) &cli_name, (socklen_t *) &cli_len);
		printf ("Assigning new socket descriptor:  %d\n", new_sd);

		if (new_sd < 0) {
			perror ("(servConn): accept() error");
			exit (-1);
		}

		Thread_Mutex_Lock(&my_pool.full_lock);

		if(my_pool.full != TRUE){
			int i;
			for(i = 0; i < POOL_SIZE; i++){
				if(my_pool.busys[i] == FALSE){
					my_pool.busys[i] = TRUE;

					do{
						Thread_Mutex_Lock(&my_pool.plock);
						if(my_pool.take == FALSE){
							Thread_Mutex_Unlock(&my_pool.plock);
							sched_yield();
							continue;
						}
						else break;
					}while(1);

					my_pool.workload = (struct my_work *)malloc(sizeof(struct my_work));
					my_pool.workload->sd = new_sd;
					my_pool.take = FALSE;
#ifdef DEBUG
					printf("%u:%X\n", my_pool.threads[i], my_pool.workload);
#endif
					Thread_Cond_Signal(&my_pool.starts[i]);

					Thread_Mutex_Unlock(&my_pool.plock);
					break;
				}
			}
			if(i == POOL_SIZE) my_pool.full = TRUE;
		}

		if(my_pool.full == TRUE){
			Thread_Mutex_Lock(&my_manager.t_lock);
			
			if(my_manager.shutdown == TRUE)
				my_manager.shutdown = FALSE;

			int num = g_slist_length(my_manager.threads);
			pthread_t *thread = NULL;
			int *temp = NULL;
			BOOL *temp2 = NULL;
			Thread_Cond_t *temp3 = NULL;
			if(num == 0){
				thread = (pthread_t *)malloc(sizeof(pthread_t));
				temp = (int *)malloc(sizeof(int));
				*temp = 0;
				temp2 = (BOOL *)malloc(sizeof(BOOL));
				*temp2 = TRUE;
				my_manager.busys = g_slist_append(my_manager.busys, temp2);
				temp3 = (Thread_Cond_t *)malloc(sizeof(Thread_Cond_t));
				Thread_Cond_Init(temp3, NULL);
				my_manager.starts = g_slist_append(my_manager.starts, temp3);

				do{
					Thread_Mutex_Lock(&my_manager.w_lock);
					if(my_manager.take == FALSE){
						Thread_Mutex_Unlock(&my_manager.w_lock);
						sched_yield();
						continue;
					}
					else break;
				}while(1);

				my_manager.workload = (struct my_work *)malloc(sizeof(struct my_work));
				my_manager.workload->sd = new_sd;
				my_manager.take = FALSE;

				Thread_Mutex_Unlock(&my_manager.w_lock);

				pthread_create(thread, NULL, do_work_2, temp);
				my_manager.threads = g_slist_append(my_manager.threads, thread);
#ifdef DEBUG
				printf("%u:%X\n", *thread, my_manager.workload);
#endif
			}
			else{
				int k;
				for(k = 0; k < num; k++)
					if(*(BOOL *)g_slist_nth_data(my_manager.busys, k) == FALSE){
						do{
							Thread_Mutex_Lock(&my_manager.w_lock);
							if(my_manager.take == FALSE){
								Thread_Mutex_Unlock(&my_manager.w_lock);
								sched_yield();
								continue;
							}
							else break;
						}while(1);

						*(BOOL *)g_slist_nth_data(my_manager.busys, k) = TRUE;
						my_manager.workload = (struct my_work *)malloc(sizeof(struct my_work));
						my_manager.workload->sd = new_sd;
						my_manager.take = FALSE;

#ifdef DEBUG
						printf("%u:%X\n", *(pthread_t *)g_slist_nth_data(my_manager.threads, k), my_manager.workload);
#endif

						Thread_Cond_Signal((Thread_Cond_t *)g_slist_nth_data(my_manager.starts, k));

						Thread_Mutex_Unlock(&my_manager.w_lock);
						break;
					}
				if(k == num){
					thread = (pthread_t *)malloc(sizeof(pthread_t));
					temp = (int *)malloc(sizeof(int));
					*temp = k;
					temp2 = (BOOL *)malloc(sizeof(BOOL));
					*temp2 = TRUE;
					my_manager.busys = g_slist_append(my_manager.busys, temp2);
					temp3 = (Thread_Cond_t *)malloc(sizeof(Thread_Cond_t));
					Thread_Cond_Init(temp3, NULL);
					my_manager.starts = g_slist_append(my_manager.starts, temp3);

					do{
						Thread_Mutex_Lock(&my_manager.w_lock);
						if(my_manager.take == FALSE){
							Thread_Mutex_Unlock(&my_manager.w_lock);
							sched_yield();
							continue;
						}
						else break;
					}while(1);

					my_manager.workload = (struct my_work *)malloc(sizeof(struct my_work));
					my_manager.workload->sd = new_sd;
					my_manager.take = FALSE;

					Thread_Mutex_Unlock(&my_manager.w_lock);

					pthread_create(thread, NULL, do_work_2, temp);
					my_manager.threads = g_slist_append(my_manager.threads, thread);
#ifdef DEBUG
					printf("%u:%X\n", *thread, my_manager.workload);
#endif
				}
			}

			Thread_Mutex_Lock(&my_manager.b_lock);

			my_manager.busy_count++;
			if(Thr_man_run == TRUE) my_manager.idle = FALSE;

			Thread_Mutex_Unlock(&my_manager.b_lock);

			Thread_Mutex_Unlock(&my_manager.t_lock);
		}

		Thread_Mutex_Unlock(&my_pool.full_lock);
	}
}

/* argv[1]: port number, argv[2]: M for activate dispatcher, argv[3]: T for dynamically created threads clearance */
int main(int argc, char **argv)
{
	int port;
	if(argc < 2) return 0;
	else port = atoi(argv[1]);
	if(argc > 2)
		my_buffer.m = atoi(argv[2]);
	if(argc > 3)
		my_manager.time = atoi(argv[3]);

	/* Initialize thread pool */
	int i;
	for(i = 0; i < POOL_SIZE; i++){
		int *temp = (int *)malloc(sizeof(int));
		*temp = i;
		pthread_create(&my_pool.threads[i], NULL, do_work, temp);
		Thread_Cond_Init(&my_pool.starts[i], NULL);
		my_pool.busys[i] = FALSE;
	}

	/* Initialize dispatcher thread */
	pthread_create(&Thread_disp, NULL, dispatch, NULL);
	pthread_create(&Thread_man, NULL, clear, NULL);

	/* Set connection */
	servConn(port);

	return 0;
}
