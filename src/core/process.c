/*
    Copyright (C) 2014 bo.shen. All Rights Reserved.
    Author: bo.shen <sanpoos@gmail.com>
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "shark.h"
#include "env.h"
#include "log.h"
#include "net.h"
#include "shm.h"
#include "util.h"
#include "spinlock.h"
#include "netevent.h"
#include "coro_sched.h"
#include "sys_signal.h"

#include "process.h"

#define INVALID_PID         -1

struct process
{
    pid_t pid;      // INVALID_PID表示不存在
    int cpuid;      // 绑定cpuid, 初始化后不变
};

static master_init_proc g_master_init_proc = NULL;
static worker_init_proc g_worker_init_proc = NULL;
static request_handler g_request_handler = request_default_handler;

static int g_listenfd;
static spinlock *g_accept_lock;    //accept fd自旋锁

static struct process g_process[MAX_WORKER_PROCESS];//子进程信息, 最多支持32个子进程
static int g_master_pid;
static int g_process_pid;          //进程id, 包含master进程
enum PROC_TYPE g_process_type;     //进程类型

static int g_conn_count = 1;       //初始化为1, 为0的时候, worker退出
static int g_all_workers_exit = 0; //workers是否都退出
static int g_create_worker = 1;    //当worker异常退出时候

int g_stop_shark = 0;       //worker是否停止接收fd, 0表示否, 其他表示停止
int g_exit_shark = 0;       //是否退出shark系统

static int worker_empty()
{
    int i;

    for (i = 0; i < g_worker_processes; i++)
    {
        struct process *p = &g_process[i];
        if (p->pid != INVALID_PID)
            return 0;
    }

    return 1;
}

static pid_t fork_worker(struct process *p)
{
    pid_t pid = fork();

    switch (pid)
    {
        case -1:
            ERR("Failed to fork worker process. master pid:%d", getpid());
            return -1;

        case 0:
            g_process_pid = getpid();
            g_process_type = WORKER_PROCESS;
            set_proc_title("shark: worker process");

            if (log_worker_alloc(g_process_pid) < 0)
            {
                printf("Failed to alloc log for process:%d\n", g_process_pid);
                exit(0);
            }

            if (bind_cpu(p->cpuid))
            {
                ERR("Failed to bind cpu: %d\n", p->cpuid);
                exit(0);
            }

            return 0;

        default:
            p->pid = pid;
            return pid;
    }
}

static void spawn_worker_process()
{
    int i;

    for (i = 0; i < g_worker_processes; i++)
    {
        struct process *p = &g_process[i];
        if (p->pid != INVALID_PID)
            continue;

        if (0 == fork_worker(p))
            break;
    }
}

static inline void increase_conn()
{
    g_conn_count++;
}

static inline void decrease_conn_and_check()
{
    if (--g_conn_count == 0)
        exit(0);
}

static void handle_connection(void *args)
{
    int connfd = (int)(intptr_t)args;

    g_request_handler(connfd);
    close(connfd);
    decrease_conn_and_check();
}

static inline int worker_can_accept()
{
    return g_conn_count < g_worker_connections;
}

static int worker_accept()
{
    struct sockaddr addr;
    socklen_t addrlen;
    int connfd;

    if (likely(g_worker_processes > 1))
    {
        if (worker_can_accept() && spin_trylock(g_accept_lock))
        {
            connfd = accept(g_listenfd, &addr, &addrlen);
            spin_unlock(g_accept_lock);
            return connfd;
        }

        return 0;
    }
    else
        connfd = accept(g_listenfd, &addr, &addrlen);

    return connfd;
}

static void worker_accept_cycle(void *args)
{
    int connfd;

    for (;;)
    {
        if (unlikely(g_stop_shark))
        {
            set_proc_title("shark: worker process is shutting down");
            decrease_conn_and_check();
            break;
        }

        if (unlikely(g_exit_shark))
            exit(0);

        connfd = worker_accept();
        if (likely(connfd > 0))
        {
            if (dispatch_coro(handle_connection, (void *)(intptr_t)connfd))
            {
                WARN("system busy to handle request.");
                close(connfd);
                continue;
            }
            increase_conn();
        }
        else if (connfd == 0)
        {
            schedule_timeout(200);
            continue;
        }
    }
}

void worker_process_cycle()
{
    if (g_worker_init_proc && g_worker_init_proc())
    {
        ERR("Failed to init worker");
        exit(0);
    }

    schedule_init(g_coro_stack_kbytes, g_worker_connections);
    event_loop_init(g_worker_connections);
    dispatch_coro(worker_accept_cycle, NULL);
    INFO("worker success running....");
    schedule_cycle();
}

static void send_signal_to_workers(int signo)
{
    int i;

    for (i = 0; i < g_worker_processes; i++)
    {
        struct process *p = &g_process[i];
        if (p->pid != INVALID_PID)
        {
            if (kill(p->pid, signo) == -1)
                ERR("Failed to send signal %d to child pid:%d", signo, p->pid);
        }
    }
}

void master_process_cycle()
{
    if (g_master_init_proc && g_master_init_proc())
    {
        ERR("Failed to init master process, shark exit");
        exit(0);
    }

    INFO("master success running....");

    for (;;)
    {
        if (g_stop_shark == 1)
        {
            WARN("notify worker processes to stop");
            send_signal_to_workers(SHUTDOWN_SIGNAL);
            g_stop_shark = 2;
        }

        if (g_exit_shark == 1)
        {
            WARN("notify worker processes to direct exit");
            send_signal_to_workers(TERMINATE_SIGNAL);
            g_exit_shark = 2;
        }

        if (g_all_workers_exit == 1)
        {
            WARN("shark exit now...");
            log_scan_write();
            delete_pidfile();
            exit(0);
        }

        if (g_create_worker)
        {
            g_create_worker = 0;
            spawn_worker_process();
            if (g_process_pid != g_master_pid)
                break;
        }

        log_scan_write();
        usleep(10000);
    }
}

void worker_exit_handler(int pid)
{
    int i;

    for (i = 0; i < g_worker_processes; i++)
    {
        struct process *p = &g_process[i];
        if (p->pid == pid)
            p->pid = INVALID_PID;
    }

    //worker进程退出, 但是并没有收到要shark停止或退出的指令, 表明子进程异常退出
    if (!g_stop_shark && !g_exit_shark)
        g_create_worker = 1;

    if (worker_empty() && (g_stop_shark || g_exit_shark))
        g_all_workers_exit = 1;
}

void register_project(master_init_proc master_proc, worker_init_proc worker_proc, request_handler handler)
{
    g_master_init_proc = master_proc;
    g_worker_init_proc = worker_proc;
    g_request_handler = handler;
}

static int create_tcp_server(const char *ip, int port)
{
    int listenfd;
    struct sockaddr_in svraddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1)
    {
        printf("socket failed. %d %s\n", errno, strerror(errno));
        exit(0);
    }

    if (set_reuse_addr(listenfd))
    {
        printf("set reuse listen socket failed. %d %s\n", errno, strerror(errno));
        exit(0);
    }

    if (set_nonblock(listenfd))
    {
        printf("set listen socket non-bloack failed. %d %s\n", errno, strerror(errno));
        exit(0);
    }

    memset(&svraddr, 0, sizeof(svraddr));
    svraddr.sin_family = AF_INET;
    svraddr.sin_port = htons(port);
    svraddr.sin_addr.s_addr = ip_to_nl(ip);

    if (0 != bind(listenfd, (struct sockaddr *)&svraddr, sizeof(svraddr)))
    {
        printf("bind failed. %d %s\n", errno, strerror(errno));
        exit(0);
    }

    if (0 != listen(listenfd, 1000))
    {
        printf("listen failed. %d %s\n", errno, strerror(errno));
        exit(0);
    }

    return listenfd;
}

void tcp_srv_init()
{
    g_listenfd = create_tcp_server(g_server_ip, g_server_port);
    g_accept_lock = shm_alloc(sizeof(spinlock));
    if (NULL == g_accept_lock)
    {
        printf("Failed to alloc global accept lock\n");
        exit(0);
    }
}

void process_init()
{
    int i;

    g_process_pid = getpid();
    g_master_pid = g_process_pid;
    g_process_type = MASTER_PROCESS;
    set_proc_title("shark: master process");
    create_pidfile(g_master_pid);
    if (log_worker_alloc(g_process_pid) < 0)
    {
        printf("Failed to alloc log for process:%d\n", g_process_pid);
        exit(0);
    }

    for (i = 0; i < g_worker_processes; i++)
    {
        struct process *p = &g_process[i];
        p->pid = INVALID_PID;
        p->cpuid = i % CPU_NUM;
    }
}

