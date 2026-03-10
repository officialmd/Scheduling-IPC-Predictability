/*
setting a: baseline ipc experiment, no inheritance, no aps.

same as v1 but now we added a logging system so we can record
state transitions for each thread. this lets us compare our
results against the paper's figure 2a and listings 1 and 2.

the log captures: time, thread id, event name, and priority.
we sort and print it at the end as an execution trace.

thread parameters (period ms, wcet ms, offset ms, priority):
  c1 = (200, 10,  0, 236)
  c2 = (200, 10,  6, 239)
  c3 = (200, 10, 12, 241)
  c4 = (200, 10, 18, 240)
  s1 = priority 238
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>

#define RUN_DURATION_S  5
#define NUM_CLIENTS     4
#define NS_TO_MS(ns)    ((double)(ns) / 1000000.0)
#define SERVER_PRIORITY 238

static const int client_params[NUM_CLIENTS][4] = {
    {200, 10,  0, 236},
    {200, 10,  6, 239},
    {200, 10, 12, 241},
    {200, 10, 18, 240},
};

static volatile int    g_chid    = -1;
static volatile int    g_coid    = -1;
static volatile int    g_running = 1;
static struct timespec g_start;

// message the client sends to the server
typedef struct {
    uint16_t type;
    uint16_t subtype;
    int      client_id;
} client_msg_t;

// reply the server sends back
typedef struct {
    int ack;
} server_reply_t;

// one log entry records a single state transition
#define LOG_MAX 8192

typedef struct {
    double time_ms;
    int    thread_id;   // 0 = server, 1..4 = clients
    char   event[32];
    int    priority;
} log_entry_t;

static log_entry_t     g_log[LOG_MAX];
static volatile int    g_log_idx = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;


/*
elapsed_ms

input: nothing
output: double, ms since experiment started
*/
static double elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = (long long)(now.tv_sec  - g_start.tv_sec)  * 1000000000LL
                 + (long long)(now.tv_nsec - g_start.tv_nsec);
    return NS_TO_MS(ns);
}

static void sleep_ms(long long ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000LL;
    nanosleep(&ts, NULL);
}

/*
burn_cpu_ms

input: ms, how long to busy wait
output: nothing

simulates cpu work by busy waiting instead of sleeping.
*/
static void burn_cpu_ms(long long ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long long target_ns = ms * 1000000LL;
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (((long long)(now.tv_sec  - start.tv_sec)  * 1000000000LL
            + (long long)(now.tv_nsec - start.tv_nsec)) < target_ns);
}

/*
log_event

input: thread_id (0=server, 1-4=clients), event name string, priority
output: nothing

appends one state transition to the log. uses a mutex so threads
dont overwrite each other.
*/
static void log_event(int thread_id, const char *event, int priority) {
    pthread_mutex_lock(&g_log_mutex);
    int idx = g_log_idx;
    if (idx < LOG_MAX) {
        g_log[idx].time_ms   = elapsed_ms();
        g_log[idx].thread_id = thread_id;
        strncpy(g_log[idx].event, event, sizeof(g_log[idx].event) - 1);
        g_log[idx].event[sizeof(g_log[idx].event) - 1] = '\0';
        g_log[idx].priority  = priority;
        g_log_idx++;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/*
print_log

input: nothing
output: nothing

sorts log entries by time and prints them as a table.
uses insertion sort since the log is small enough.
*/
static void print_log(void) {
    printf("\n========== execution trace ==========\n");
    printf("%-14s %-8s %-20s %-8s\n", "time(ms)", "thread", "event", "priority");
    printf("%-14s %-8s %-20s %-8s\n", "--------", "------", "-----", "--------");

    for (int i = 1; i < g_log_idx; i++) {
        log_entry_t key = g_log[i];
        int j = i - 1;
        while (j >= 0 && g_log[j].time_ms > key.time_ms) {
            g_log[j + 1] = g_log[j];
            j--;
        }
        g_log[j + 1] = key;
    }

    for (int i = 0; i < g_log_idx; i++) {
        const char *name;
        switch (g_log[i].thread_id) {
            case 0:  name = "s1"; break;
            case 1:  name = "c1"; break;
            case 2:  name = "c2"; break;
            case 3:  name = "c3"; break;
            case 4:  name = "c4"; break;
            default: name = "??"; break;
        }
        printf("%-14.4f %-8s %-20s %-8d\n",
               g_log[i].time_ms, name, g_log[i].event, g_log[i].priority);
    }
    printf("=====================================\n");
}


/*
server_thread

input: arg, unused
output: NULL

creates a channel, loops receiving messages, simulates work,
and replies. logs RECEIVE_BLOCKED and RUNNING transitions.
*/
static void *server_thread(void *arg) {
    (void)arg;

    g_chid = ChannelCreate(0);
    if (g_chid == -1) {
        perror("ChannelCreate failed");
        exit(EXIT_FAILURE);
    }

    printf("[s1] started at priority %d\n", SERVER_PRIORITY);

    client_msg_t   msg;
    server_reply_t reply;
    int            rcvid;

    while (g_running) {
        log_event(0, "RECEIVE_BLOCKED", SERVER_PRIORITY);

        rcvid = MsgReceive(g_chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            if (!g_running) break;
            perror("MsgReceive error");
            break;
        }

        log_event(0, "RUNNING", SERVER_PRIORITY);
        printf("[s1] %.4f ms  handling request from c%d\n",
               elapsed_ms(), msg.client_id);

        burn_cpu_ms(10);

        reply.ack = 1;
        MsgReply(rcvid, EOK, &reply, sizeof(reply));
        log_event(0, "READY", SERVER_PRIORITY);
    }

    ChannelDestroy(g_chid);
    return NULL;
}

// holds parameters for one client thread
typedef struct {
    int id;
    int period_ms;
    int wcet_ms;
    int offset_ms;
    int priority;
} client_arg_t;


/*
client_thread

input: arg, pointer to client_arg_t
output: NULL

each period: logs READY, burns cpu, logs SEND_BLOCKED, calls msgsend,
logs RUNNING when reply comes back, burns remaining cpu, sleeps.
*/
static void *client_thread(void *arg) {
    client_arg_t *p = (client_arg_t *)arg;

    sleep_ms(p->offset_ms);
    while (g_coid == -1) usleep(1000);

    printf("[c%d] started at priority %d\n", p->id, p->priority);

    client_msg_t   msg;
    server_reply_t reply;
    msg.type      = 0x01;
    msg.subtype   = 0x00;
    msg.client_id = p->id;

    while (g_running) {
        log_event(p->id, "READY", p->priority);
        burn_cpu_ms(p->wcet_ms / 2);

        log_event(p->id, "SEND_BLOCKED", p->priority);
        int ret = MsgSend(g_coid, &msg, sizeof(msg), &reply, sizeof(reply));
        if (ret == -1) {
            if (!g_running) break;
            break;
        }

        log_event(p->id, "RUNNING", p->priority);
        burn_cpu_ms(p->wcet_ms / 2);

        long long remaining = (long long)p->period_ms - (long long)p->wcet_ms;
        if (remaining > 0) sleep_ms(remaining);
    }

    return NULL;
}


/*
main

input: nothing
output: 0 on success

starts the server, connects to its channel, starts all 4 clients,
waits for the experiment, then prints the log.
*/
int main(void) {
    printf("=== setting a: added logging ===\n\n");

    clock_gettime(CLOCK_MONOTONIC, &g_start);

    pthread_t          server_tid;
    pthread_attr_t     server_attr;
    struct sched_param server_sp;

    pthread_attr_init(&server_attr);
    pthread_attr_setinheritsched(&server_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&server_attr, SCHED_FIFO);
    server_sp.sched_priority = SERVER_PRIORITY;
    pthread_attr_setschedparam(&server_attr, &server_sp);
    pthread_create(&server_tid, &server_attr, server_thread, NULL);
    pthread_attr_destroy(&server_attr);

    while (g_chid == -1) usleep(1000);

    g_coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_coid == -1) {
        perror("ConnectAttach failed");
        exit(EXIT_FAILURE);
    }

    pthread_t    client_tids[NUM_CLIENTS];
    client_arg_t client_args[NUM_CLIENTS];

    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_args[i].id        = i + 1;
        client_args[i].period_ms = client_params[i][0];
        client_args[i].wcet_ms   = client_params[i][1];
        client_args[i].offset_ms = client_params[i][2];
        client_args[i].priority  = client_params[i][3];

        pthread_attr_t     attr;
        struct sched_param sp;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sp.sched_priority = client_params[i][3];
        pthread_attr_setschedparam(&attr, &sp);
        pthread_create(&client_tids[i], &attr, client_thread, &client_args[i]);
        pthread_attr_destroy(&attr);
    }

    sleep(RUN_DURATION_S);
    g_running = 0;
    sleep(1);

    print_log();
    ConnectDetach(g_coid);
    return EXIT_SUCCESS;
}
