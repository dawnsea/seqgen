/*
 * seqgen, high speeed sequence number generator
 * build : make
 * who : dawnsea, keeptalk@gmail.com
 * There is Original comment box below this comment.
 * The other original comments was removed for reading codes.
 */

 /**
  * Multithreaded, libevent-based socket server.
  * Copyright (c) 2012 Ronald Bennett Cemer
  * This software is licensed under the BSD license.
  * See the accompanying LICENSE.txt for details.
  *
  * To compile: gcc -o echoserver_threaded echoserver_threaded.c workqueue.c -levent -lpthread
  * To run: ./echoserver_threaded
  */

/*
 * 최초 기동후 마스터로 지정한 측과 커넥트가 성공만 하면 무조건 슬레이브 모드로 동작한다.
 * 슬레이브 모드에서는 무조건 마스터를 호출하여 쓰리쿠션으로 서빙한다.
 * 슬레이브 모드로 동작하다가 마스터 측에서 응답이 없으면 바로 스스로 마스터로 이행하여 서빙한다.
 * 이후 죽었던 마스터가 복귀되면 원래 슬레이브였던 현재 마스터에 접속을 시도해보고 접속하면 이전의 마스터는 슬레이브가 된다.
 * 쉽죠?
 */


#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <event.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <string.h>
#include <netinet/tcp.h>


#include "workqueue.h"

#define HOSTNAME_LEN        255
#define PATH_LEN         	255
#define TIMEOUT_KEEPALIVE   10

#define DAEMON_NAME 		"seqgen"

#define MODE_HTTP			0
#define MODE_SOCK			1
#define MODE_MC				2

#define DEF_PID_FILE		"/var/lock/seqgen.pid"
#define DEF_LOG_FILE		"/tmp/debug.log"
#define DEF_SRV_PORT		5555
#define DEF_BACKLOG			1024		// backlog
#define DEF_WORKER			100		// thread
#define DEF_KL_INIT			1000
#define DEF_KL_TOUT			1

static int 				pid_fd;
static volatile int 	global_lock;

static unsigned long    global_count 	= 0;
static unsigned long    global_epoch	= 0;
static int				global_master 	= 0;

struct env_t {
    unsigned int    serve_port;
    unsigned long   init_value;
    unsigned long   init_epoch;
    unsigned int    running_mode;   // 0 : http, 1: socket, 2: memcached compatible
    unsigned int    backlog;
	unsigned int	thread_no;
	unsigned int	keepalive;
	unsigned int	keepalive_init_count;
	unsigned int	keepalive_read_timeout;
	char			log_path[PATH_LEN + 1];
	char 			pid_file[PATH_LEN + 1];
    char            master_addr[HOSTNAME_LEN + 1];
	int				master_port;
};

struct env_t env;

typedef struct client {
	int fd;
	struct event_base 	*evbase;
	struct bufferevent 	*buf_ev;
	struct evbuffer 	*output_buffer;
	unsigned long 		last_time;
	unsigned int		keepalive_count;
	unsigned int		keepalive;
	int					master_fd;

} client_t;

static struct event_base 	*evbase_accept;
static workqueue_t 			workqueue;

static void conn_to_master(client_t *client);

static void disp_params(void)
{
    printf( "port         = %d\n"
            "init value   = %ld\n"
            "init epoch   = %ld\n"
            "running mode = %s\n"
			"thread       = %d\n"
			"keepalive    = %d\n"
			" age         = %d\n"
			" timeout     = %d\n"
            "master addr  = %s\n"
			"master port  = %d\n"
//			"log path     = %s\n"
			"pid file     = %s\n"
            "backlog      = %d\n",
			env.serve_port,
			env.init_value,
			env.init_epoch,
			env.running_mode == MODE_SOCK ? "socket" : env.running_mode == MODE_MC ? "memcached" : "http",
			env.thread_no,
			env.keepalive,
			env.keepalive_init_count,
			env.keepalive_read_timeout,
			env.master_addr,
			env.master_port,
//			env.log_path,
			env.pid_file, env.backlog);
}

static inline void simple_spinlock(void)
{
    while (__sync_val_compare_and_swap(&global_lock, 0, 1));
    asm volatile("lfence" : : : "memory");
}

static inline void simple_spinunlock(void) {
    global_lock = 0;
	asm volatile("sfence" : : : "memory");
}

static void parse_params(int argc, char *argv[])
{
    char c;

	env.serve_port		= DEF_SRV_PORT;
	env.backlog			= DEF_BACKLOG;
	env.thread_no		= DEF_WORKER;
	env.init_epoch		= 0;
	env.init_value		= 0;
	env.running_mode 	= 0; // httpd
	env.keepalive		= 1;
	env.keepalive_init_count 	= DEF_KL_INIT;
	env.keepalive_read_timeout 	= DEF_KL_TOUT;
	env.master_port		= 5555;

	strcpy(env.log_path, DEF_LOG_FILE);
	strcpy(env.pid_file, DEF_PID_FILE);

    while (-1 != (c = getopt(argc, argv,
         "p:"   // 포트
         "i:"   // 초기값
         "e:"   // 에포크 시작 값
         "m:"	// 마스터 주소
		 "d:"	//
		 "o:"
		 "n"	// keepalive
		 "hsc"
     ))) {
    switch (c) {
        case 'p':
            env.serve_port = atoi(optarg);
            if (env.serve_port <= 0)
                err(-1, "port error");
            break;
        case 'i':
            env.init_value = atol(optarg);
            global_count   = env.init_value;
            break;
        case 'e':
            env.init_epoch = atol(optarg);
            global_epoch   = env.init_epoch;
            break;
        case 'h':		// httpd
            env.running_mode = MODE_HTTP;
            break;
        case 's':		// socket
            env.running_mode = MODE_SOCK;
            break;
		case 'c':		// memcached
            env.running_mode = MODE_MC;
            break;
        case 'm':
            strncpy(env.master_addr, optarg, HOSTNAME_LEN);
            break;
		case 'o':
            env.master_port = atoi(optarg);
	        break;
		case 'n':
            env.keepalive = 0;
            break;
		case 'd':
            strncpy(env.log_path, 	optarg, 		PATH_LEN);
			strncat(env.log_path, 	"debug.log", 	PATH_LEN);
            break;
        default:
            break;
         }
     }
}

static void killServer(void)
{
	syslog(LOG_INFO, "Stopping socket listener event loop.\n");
	if (event_base_loopexit(evbase_accept, NULL)) {
		syslog(LOG_ERR, "Error shutting down server");
	}
	syslog(LOG_INFO, "Stopping workers.\n");
	workqueue_shutdown(&workqueue);
}

static void signal_handler(int sig)
{
	 switch(sig) {
		 case SIGHUP:
			 syslog(LOG_WARNING, "Received SIGHUP signal.");
			 break;
		 case SIGINT:
		 case SIGTERM:
			 syslog(LOG_INFO, "Daemon exiting");
			 killServer();
			 close(pid_fd);
			 exit(EXIT_SUCCESS);
			 break;
		 default:
			 syslog(LOG_WARNING, "Unhandled signal %s", strsignal(sig));
			 break;
	 }
}

static void go_daemon(void)
{
	int i;
	struct sigaction new_sigaction;
	sigset_t sigblock;
	char str[10];

	if (getppid() == 1)
		err(1, "already daemon");

	sigemptyset(&sigblock);
	sigaddset(&sigblock, SIGCHLD);
	sigaddset(&sigblock, SIGTSTP);
	sigaddset(&sigblock, SIGTTOU);
	sigaddset(&sigblock, SIGTTIN);
	sigprocmask(SIG_BLOCK, &sigblock, NULL);

	new_sigaction.sa_handler = signal_handler;
	sigemptyset(&new_sigaction.sa_mask);
	new_sigaction.sa_flags = 0;

	/* Signals to handle */
	sigaction(SIGHUP, 	&new_sigaction, NULL);     /* catch hangup signal */
	sigaction(SIGTERM, 	&new_sigaction, NULL);    /* catch term signal */
	sigaction(SIGINT, 	&new_sigaction, NULL);     /* catch interrupt signal */

    switch (fork()) {
        case -1:
            err(1, "forking error");
        case 0:
            break;
        default:
            _exit(EXIT_SUCCESS);
    }

	umask(027);

    if (setsid() == -1)
        err(1, "session get error");

    for (i = getdtablesize(); i >= 0; --i)
		close(i);

    if(chdir("/") != 0)
        err(1, "chdir error");

	i = open("/dev/null", O_RDWR);

	if (dup(i) == -1)
		err(1, "stdout dup error");

	if (dup(i) == -1)
		err(1, "stdin dup error");

	 pid_fd = open(env.pid_file, O_RDWR|O_CREAT, 0600);

	 if (pid_fd == -1 ) {
		 /* Couldn't open lock file */
		 syslog(LOG_INFO, "Could not open PID lock file %s, exiting", env.pid_file);
		 exit(EXIT_FAILURE);
	 }

	 /* Try to lock file */
	 if (lockf(pid_fd, F_TLOCK,0) == -1) {
		 /* Couldn't get lock on lock file */
		 syslog(LOG_INFO, "Could not lock PID lock file %s, exiting", env.pid_file);
		 exit(EXIT_FAILURE);
	 }

	 /* Get and format PID */
	 sprintf(str,"%d\n",getpid());

	 /* write pid to lockfile */
	 i = write(pid_fd, str, strlen(str));
	 if (i < 0) {
		 syslog(LOG_ERR, "pid file write error");
		 exit(EXIT_FAILURE);
	 }

}

static int setnonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0) return flags;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) return -1;
	return 0;
}

static void closeClient(client_t *client)
{
	if (client != NULL) {
		if (client->fd >= 0) {
			close(client->fd);
			client->fd = -1;
		}
	}
}

static void closeEvent(client_t *client)
{
	if (client->buf_ev != NULL) {
		bufferevent_free(client->buf_ev);
		client->buf_ev = NULL;
	}
	if (client->evbase != NULL) {
		event_base_free(client->evbase);
		client->evbase = NULL;
	}
	if (client->output_buffer != NULL) {
		evbuffer_free(client->output_buffer);
		client->output_buffer = NULL;
	}
}

static void closeAndFreeClient(client_t *client)
{
	if (client != NULL) {
		closeClient(client);
		closeEvent(client);
		free(client);
	}
}

#define BUF_MAX	4096
void buffered_on_read(struct bufferevent *bev, void *arg) {
	client_t *client = (client_t *)arg;
	int wlen, len;
	char data[BUF_MAX + 1], hb = 'H';
	int nbytes;
	int res;
	fd_set fds;
	struct timeval tv;
	socklen_t lon;
	int valopt;

	while ((nbytes = EVBUFFER_LENGTH(bev->input)) > 0) {
		if (nbytes > BUF_MAX) nbytes = BUF_MAX;
		evbuffer_remove(bev->input, data, nbytes);

		if (nbytes > 30 && strcasestr(data, "Connection: Keep-Alive") != NULL) {
			client->keepalive = 1;
		}
#if 0
//		evbuffer_drain(bev->input, nbytes);
		simple_spinlock();
		if (global_master == 0) {
			simple_spinunlock();

			res = write(client->master_fd, &hb, 1);
			if (res < 0) {
				if (errno == EINPROGRESS) {
					tv.tv_sec 	= 1;
					tv.tv_usec 	= 0;

					FD_ZERO(&fds);
					FD_SET(client->master_fd, &fds);

					if (select(client->master_fd + 1, NULL, &fds, NULL, &tv) > 0) {
						lon = sizeof(int);
						getsockopt(client->master_fd, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon);
						if (valopt) {
							close(client->master_fd);
							simple_spinlock();
							global_master = 1;
							simple_spinunlock();
						}
					} else {
						close(client->master_fd);
						simple_spinlock();
						global_master = 1;
						simple_spinunlock();
					}
				}
			}


		} else {
			simple_spinunlock();
		}

		res = read(client->master_fd, data, BUF_MAX);
		if (res < 0) {
			if (errno == EINPROGRESS) {
				tv.tv_sec 	= 1;
				tv.tv_usec 	= 0;

				FD_ZERO(&fds);
				FD_SET(client->master_fd, &fds);

				if (select(client->master_fd + 1, &fds, NULL, NULL, &tv) > 0) {
					lon = sizeof(int);
					getsockopt(client->master_fd, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon);
					if (valopt) {
						close(client->master_fd);
						simple_spinlock();
						global_master = 1;
						simple_spinunlock();
					}
				} else {
					close(client->master_fd);
					simple_spinlock();
					global_master = 1;
					simple_spinunlock();
				}
			}




		}
#endif
		simple_spinlock();
		switch (env.running_mode) {
			case MODE_HTTP:
				if (env.keepalive && client->keepalive_count > 0 && client->keepalive) {
					snprintf(data, BUF_MAX, "HTTP/1.1 200 OK\r\nKeep-Alive: timeout=%d, max=%d\r\nConnection: Keep-Alive\r\nContent-Length: 33\r\n\r\n%016lx:%016lx",
						env.keepalive_read_timeout, client->keepalive_count, global_epoch, global_count);
				} else {
					snprintf(data, BUF_MAX, "HTTP/1.1 200 OK\r\nContent-Length: 33\r\nConnection: close\r\n\r\n%016lx:%016lx",
						global_epoch, global_count);
				}

				break;
			case MODE_SOCK:
				snprintf(data, BUF_MAX, "%016lx:%016lx", global_epoch, global_count);
				break;
			case MODE_MC:
				snprintf(data, BUF_MAX, "%ld\r\n", global_count);
				break;

		}
		simple_spinunlock();
		evbuffer_add(client->output_buffer, data, strnlen(data, BUF_MAX));
	}

	if (bufferevent_write_buffer(bev, client->output_buffer)) {
		syslog(LOG_INFO, "Error sending data to client on fd %d\n", client->fd);
		closeClient(client);
	}
}

void buffered_on_write(struct bufferevent *bev, void *arg)
{
	client_t *client = (client_t *)arg;

	simple_spinlock();
	if (global_count == 0xffffffffffffffffLL) {
		global_count = 0;
		global_epoch++;
	} else {
		global_count++;
	}
	simple_spinunlock();

	if (env.running_mode == MODE_HTTP) {
		if (env.keepalive && client->keepalive) {
			if (client->keepalive_count == 0) {
				bufferevent_set_timeouts(client->buf_ev, NULL, NULL);
				event_base_loopexit(client->evbase, NULL);
//				closeAndFreeClient(client);
//				closeClient(client);
//				client->keepalive_count = env.keepalive_init_count;
			} else {
				client->keepalive_count--;
			}
		} else {
			bufferevent_set_timeouts(client->buf_ev, NULL, NULL);
			event_base_loopexit(client->evbase, NULL);
//			closeAndFreeClient(client);
//			closeClient(client);
		}
	}
}


void buffered_on_error(struct bufferevent *bev, short what, void *arg) {
	closeClient((client_t *)arg);
}

static void server_job_function(struct job *job) {
	client_t *client = (client_t *)job->user_data;

	event_base_dispatch(client->evbase);
	closeAndFreeClient(client);
	free(job);
}

void on_accept(int fd, short ev, void *arg) {
	int client_fd;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	workqueue_t *workqueue = (workqueue_t *)arg;
	client_t *client;
	job_t *job;
	struct timeval tv;

	client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd < 0) {
		syslog(LOG_WARNING, "accept failed");
		return;
	}

	if (setnonblock(client_fd) < 0) {
		syslog(LOG_WARNING, "failed to set client socket to non-blocking");
		close(client_fd);
		return;
	}

	if ((client = malloc(sizeof(*client))) == NULL) {
		syslog(LOG_WARNING, "failed to allocate memory for client state");
		close(client_fd);
		return;
	}
	memset(client, 0, sizeof(*client));
	client->fd = client_fd;
//	conn_to_master(client);

	if (env.running_mode == MODE_HTTP && env.keepalive) {
		tv.tv_sec 	= env.keepalive_read_timeout;
		tv.tv_usec 	= 0;
		client->keepalive_count = env.keepalive_init_count;
	}

	if ((client->output_buffer = evbuffer_new()) == NULL) {
		syslog(LOG_WARNING, "client output buffer allocation failed");
		closeAndFreeClient(client);
		return;
	}

	if ((client->evbase = event_base_new()) == NULL) {
		syslog(LOG_WARNING, "client event_base creation failed");
		closeAndFreeClient(client);
		return;
	}

	if ((client->buf_ev = bufferevent_new(client_fd, buffered_on_read, buffered_on_write, buffered_on_error, client)) == NULL) {
		syslog(LOG_WARNING, "client bufferevent creation failed");
		closeAndFreeClient(client);
		return;
	}
	bufferevent_base_set(client->evbase, client->buf_ev);

	if (env.running_mode == MODE_HTTP && env.keepalive)
		bufferevent_set_timeouts(client->buf_ev, &tv, NULL);

//	bufferevent_settimeout(client->buf_ev, SOCKET_READ_TIMEOUT_SECONDS, SOCKET_WRITE_TIMEOUT_SECONDS);

	bufferevent_enable(client->buf_ev, EV_READ);

	if ((job = malloc(sizeof(*job))) == NULL) {
		syslog(LOG_WARNING, "failed to allocate memory for job state");
		closeAndFreeClient(client);
		return;
	}
	job->job_function = server_job_function;
	job->user_data = client;

	workqueue_add_job(workqueue, job);
}

int runServer(void)
{
	int listenfd;
	struct sockaddr_in listen_addr;
	struct event ev_accept;
	int reuseaddr_on;

	event_init();

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0) {
		syslog(LOG_ERR, "listen failed");
	}

	reuseaddr_on = 1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_on, sizeof(reuseaddr_on));

	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_addr.s_addr = INADDR_ANY;
	listen_addr.sin_port = htons(env.serve_port);
	if (bind(listenfd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
		syslog(LOG_ERR, "bind failed");
	}

	if (listen(listenfd, env.backlog) < 0) {
		syslog(LOG_ERR, "listen failed");
	}

	if (setnonblock(listenfd) < 0) {
		syslog(LOG_ERR, "failed to set server socket to non-blocking");
	}

	if ((evbase_accept = event_base_new()) == NULL) {
		syslog(LOG_ERR, "Unable to create socket accept event base");
		close(listenfd);
		return 1;
	}

	if (workqueue_init(&workqueue, env.thread_no)) {
		syslog(LOG_ERR, "Failed to create work queue");
		close(listenfd);
		workqueue_shutdown(&workqueue);
		return 1;
	}

	event_set(&ev_accept, listenfd, EV_READ|EV_PERSIST, on_accept, (void *)&workqueue);
	event_base_set(evbase_accept, &ev_accept);
	event_add(&ev_accept, NULL);

	syslog(LOG_INFO, "Server running.\n");

	event_base_dispatch(evbase_accept);

	event_base_free(evbase_accept);
	evbase_accept = NULL;

	close(listenfd);
	syslog(LOG_INFO, "Server shutdown.\n");

	return 0;
}

int tcpSetKeepAlive(int nSockFd_, int nKeepAlive_, int nKeepAliveIdle_, int nKeepAliveCnt_, int nKeepAliveInterval_)
{
     int nRtn;
     nRtn = setsockopt(nSockFd_, SOL_SOCKET, SO_KEEPALIVE, &nKeepAlive_, sizeof(nKeepAlive_));
     if(nRtn == -1)
    {
         err(1, "[TCP server]Fail: setsockopt():so_keepalive");
         return -1;
     }
     nRtn = setsockopt(nSockFd_, SOL_TCP, TCP_KEEPIDLE, &nKeepAliveIdle_, sizeof(nKeepAliveIdle_) );
     if(nRtn == -1)
     {
          err(1, "[TCP server]Fail: setsockopt():so_keepidle");
          return -1;
     }
     nRtn = setsockopt(nSockFd_, SOL_TCP, TCP_KEEPCNT, &nKeepAliveCnt_, sizeof(nKeepAliveCnt_) );
     if(nRtn == -1)
     {
          err(1, "[TCP server]Fail: setsockopt():so_keepcnt");
          return -1;
      }
     nRtn = setsockopt(nSockFd_, SOL_TCP, TCP_KEEPINTVL, &nKeepAliveInterval_, sizeof(nKeepAliveInterval_) );
     if(nRtn == -1)
     {
          err(1, "[TCP server]Fail: setsockopt():so_keepintvl");
          return -1;
      }
     return nRtn;
}

static void conn_to_master(client_t *client)
{
 	struct sockaddr_in 	serveraddr;
	struct timeval 		tv;
	fd_set 				conn_set;
	socklen_t 			lon;
	int					valopt;
	int					res;
	int					master = 0;

	client->master_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (client->master_fd == -1) {
		printf("master mode start 0\n");
		master = 1;
	}

	if (setnonblock(client->master_fd) < 0) {
		err(1, "nonblock error");
	}

	tcpSetKeepAlive(client->master_fd, 1, 1, 3, 10);

	serveraddr.sin_family 			= AF_INET;
	serveraddr.sin_addr.s_addr 		= inet_addr(env.master_addr);
	serveraddr.sin_port 			= htons(env.master_port);

	if (serveraddr.sin_addr.s_addr < 0) {
		printf("master mode start 1\n");
		master = 1;
	}

	if (master == 0) {
		res = connect(client->master_fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
		if (res < 0) {
			if (errno == EINPROGRESS) {
				tv.tv_sec 	= 1;
				tv.tv_usec 	= 0;
				FD_ZERO(&conn_set);
				FD_SET(client->master_fd, &conn_set);
				if (select(client->master_fd + 1, NULL, &conn_set, NULL, &tv) > 0) {
					lon = sizeof(int);
					getsockopt(client->master_fd, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon);
					if (valopt) {
						close(client->master_fd);
						master = 1;
						printf("master mode start 2 %s\n", valopt == ECONNREFUSED ? "ECONNREFUSED" : valopt == ETIMEDOUT ? "ETIMEDOUT" : "unknown");
					}
				} else {

					close(client->master_fd);
					master = 1;
					printf("master mode start 3\n");
				}
			}
		}
	}

	simple_spinlock();
	global_master = master;
	simple_spinunlock();
}

void init_master_check(void)
{
	client_t client;
	conn_to_master(&client);
	close(client.master_fd);
}

int main(int argc, char *argv[])
{
	parse_params(argc, argv);
	disp_params();

	if (env.master_addr[0] != 0) {
		init_master_check();
	}

	setlogmask(LOG_UPTO(LOG_INFO));
	openlog(DAEMON_NAME, LOG_CONS | LOG_PERROR, LOG_USER);

	syslog(LOG_INFO, "seqgen starting up");

//	go_daemon();
	return runServer();
}
