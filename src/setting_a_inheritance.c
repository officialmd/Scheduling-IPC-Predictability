/*
setting a with priority inheritance enabled.

sources:
- qnx ChannelCreate docs: priority inheritance can be disabled with
  _NTO_CHF_FIXED_PRIORITY
- qnx priority inheritance docs: receiver priority is boosted/lowered
  based on incoming messages

what stays the same as the baseline:
  - 4 clients, 1 server, same periods/wcet/offsets/priorities
  - 10 ms server service time
  - same 5 second runtime
  - same send/reply behavior and general logging format

what is added:
  - server priority sampling at key points
  - richer event names that show when a server priority sample was taken
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

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

static volatile int g_chid = -1;
static volatile int g_coid = -1;
static struct timespec g_start;
static volatile int g_running = 1;

#define LOG_MAX 12288

typedef struct {
    double time_ms;
    int    thread_id;
    char   event[48];
    int    priority;
} log_entry_t;

static log_entry_t     g_log[LOG_MAX];
static volatile int    g_log_idx = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint16_t type;
    uint16_t subtype;
    int      client_id;
} client_msg_t;

typedef struct {
    int ack;
} server_reply_t;

typedef struct {
    int id;
    int period_ms;
    int wcet_ms;
    int offset_ms;
    int priority;
} client_arg_t;

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

static void burn_cpu_ms(long long ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long long target_ns = ms * 1000000LL;
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (((long long)(now.tv_sec  - start.tv_sec)  * 1000000000LL
            + (long long)(now.tv_nsec - start.tv_nsec)) < target_ns);
}

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

static int current_thread_priority(void) {
    int policy;
    struct sched_param sp;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) {
        return -1;
    }
    return sp.sched_priority;
}

static void log_server_priority(const char *event) {
    log_event(0, event, current_thread_priority());
}

static void print_log(void) {
    printf("\n========== execution trace (inheritance) ==========""\n");
    printf("%-14s %-8s %-28s %-8s\n", "time(ms)", "thread", "event", "priority");
    printf("%-14s %-8s %-28s %-8s\n", "--------", "------", "-----", "--------");

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
        printf("%-14.4f %-8s %-28s %-8d\n",
               g_log[i].time_ms, name, g_log[i].event, g_log[i].priority);
    }
    printf("==================================================\n");
}

static void *server_thread(void *arg) {
    (void)arg;

    /*
    inheritance-enabled channel:
    qnx says _NTO_CHF_FIXED_PRIORITY disables priority inheritance.
    so for the inheritance experiment we create the channel with flags 0.
    */
    g_chid = ChannelCreate(0);
    if (g_chid == -1) {
        perror("ChannelCreate failed");
        exit(EXIT_FAILURE);
    }

    printf("[s1] started at base priority %d, channel id = %d (inheritance enabled)\n",
           SERVER_PRIORITY, g_chid);
    log_server_priority("STARTUP");

    client_msg_t   msg;
    server_reply_t reply;
    int            rcvid;

    while (g_running) {
        log_server_priority("RECEIVE_BLOCKED");
        rcvid = MsgReceive(g_chid, &msg, sizeof(msg), NULL);

        if (rcvid == -1) {
            if (!g_running) break;
            perror("MsgReceive error");
            break;
        }

        log_server_priority("RUNNING_AFTER_RECEIVE");
        log_event(msg.client_id, "REPLY_BLOCKED",
                  client_params[msg.client_id - 1][3]);

        printf("[s1] %.4f ms  handling request from c%d  (server eff prio=%d)\n",
               elapsed_ms(), msg.client_id, current_thread_priority());

        log_server_priority("SERVICE_BEGIN");
        burn_cpu_ms(10);
        log_server_priority("SERVICE_END");

        reply.ack = 1;
        MsgReply(rcvid, EOK, &reply, sizeof(reply));

        log_server_priority("AFTER_REPLY");
        log_server_priority("READY");
    }

    printf("[s1] shutting down\n");
    ChannelDestroy(g_chid);
    return NULL;
}

static void *client_thread(void *arg) {
    client_arg_t *p = (client_arg_t *)arg;

    sleep_ms(p->offset_ms);
    while (g_coid == -1) usleep(1000);

    printf("[c%d] started at priority %d, period=%dms, offset=%dms\n",
           p->id, p->priority, p->period_ms, p->offset_ms);

    client_msg_t   msg;
    server_reply_t reply;

    msg.type      = 0x01;
    msg.subtype   = 0x00;
    msg.client_id = p->id;

    while (g_running) {
        log_event(p->id, "READY", p->priority);
        burn_cpu_ms(p->wcet_ms / 2);

        log_event(p->id, "SEND_BLOCKED", p->priority);
        printf("[c%d] %.4f ms  send_blocked\n", p->id, elapsed_ms());

        int ret = MsgSend(g_coid, &msg, sizeof(msg), &reply, sizeof(reply));
        if (ret == -1) {
            if (!g_running) break;
            perror("MsgSend error");
            break;
        }

        log_event(p->id, "RUNNING", p->priority);
        printf("[c%d] %.4f ms  running (got reply)\n", p->id, elapsed_ms());

        burn_cpu_ms(p->wcet_ms / 2);

        long long remaining = (long long)p->period_ms - (long long)p->wcet_ms;
        if (remaining > 0) sleep_ms(remaining);
    }

    printf("[c%d] done\n", p->id);
    return NULL;
}

int main(void) {
    printf("=== setting a: ipc with priority inheritance enabled ===\n");
    printf("target: validate r8 by observing server effective priority\n\n");

    clock_gettime(CLOCK_MONOTONIC, &g_start);

    pthread_t          server_tid;
    pthread_attr_t     server_attr;
    struct sched_param server_sp;
    int                ret;

    pthread_attr_init(&server_attr);
    pthread_attr_setinheritsched(&server_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&server_attr, SCHED_FIFO);
    server_sp.sched_priority = SERVER_PRIORITY;
    pthread_attr_setschedparam(&server_attr, &server_sp);

    ret = pthread_create(&server_tid, &server_attr, server_thread, NULL);
    if (ret != 0) {
        fprintf(stderr, "failed to create server thread: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(&server_attr);

    while (g_chid == -1) usleep(1000);

    g_coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_coid == -1) {
        perror("ConnectAttach failed");
        exit(EXIT_FAILURE);
    }
    printf("[main] connected to server channel (chid=%d, coid=%d)\n\n",
           g_chid, g_coid);

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

        ret = pthread_create(&client_tids[i], &attr, client_thread,
                             &client_args[i]);
        if (ret != 0) {
            fprintf(stderr, "failed to create c%d (priority %d): %s\n",
                    i + 1, client_params[i][3], strerror(ret));
        } else {
            printf("[main] created c%d at priority %d\n",
                   i + 1, client_params[i][3]);
        }
        pthread_attr_destroy(&attr);
    }

    printf("\n[main] inheritance experiment running for %d seconds...\n\n",
           RUN_DURATION_S);

    sleep(RUN_DURATION_S);
    g_running = 0;
    sleep(1);

    print_log();
    ConnectDetach(g_coid);

    printf("key things to check for r8:\n");
    printf("  1) s1 priority should rise above its base priority %d when serving higher-priority clients\n",
           SERVER_PRIORITY);
    printf("  2) RUNNING_AFTER_RECEIVE / SERVICE_BEGIN priorities should reflect the selected client\n");
    printf("  3) AFTER_REPLY may still show inherited priority until the next MsgReceive selects a new request\n");
    printf("  4) verify the strongest evidence with System Profiler / kernel trace\n");

    return EXIT_SUCCESS;
}
