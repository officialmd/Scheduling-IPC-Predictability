/*
setting a: baseline ipc experiment, no inheritance, no aps.

we have 4 clients and 1 server. each client does some cpu work
then sends a message to the server and waits for a reply.
we are replicating the paper: becker et al., ieee rtas 2023.

thread parameters (period ms, wcet ms, offset ms, priority):
  c1 = (200, 10,  0, 236)
  c2 = (200, 10,  6, 239)
  c3 = (200, 10, 12, 241)
  c4 = (200, 10, 18, 240)
  s1 = priority 238

note: priorities are shifted down 14 from the paper so we dont
need root. the relative ordering is the same.
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


/*
elapsed_ms

input: nothing
output: double, ms since experiment started

returns how many milliseconds have passed since g_start was set.
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

simulates cpu work by busy waiting. we use this instead of sleep
because sleep yields the cpu which is not the same as real computation.
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
server_thread

input: arg, unused
output: NULL

creates a channel, then loops receiving messages from clients,
simulating 10ms of work, and replying.
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
        rcvid = MsgReceive(g_chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            if (!g_running) break;
            perror("MsgReceive error");
            break;
        }

        printf("[s1] %.4f ms  handling request from c%d\n",
               elapsed_ms(), msg.client_id);

        burn_cpu_ms(10);

        reply.ack = 1;
        MsgReply(rcvid, EOK, &reply, sizeof(reply));
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

waits for its offset, then each period: burns cpu for half wcet,
sends a message to the server, waits for reply, burns remaining cpu,
then sleeps for the rest of the period.
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
        burn_cpu_ms(p->wcet_ms / 2);

        printf("[c%d] %.4f ms  sending\n", p->id, elapsed_ms());

        int ret = MsgSend(g_coid, &msg, sizeof(msg), &reply, sizeof(reply));
        if (ret == -1) {
            if (!g_running) break;
            break;
        }

        printf("[c%d] %.4f ms  got reply\n", p->id, elapsed_ms());

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

starts the server, waits for it to create its channel, connects to
it, then starts all 4 client threads and waits for the experiment.
*/
int main(void) {
    printf("=== setting a v1: basic ipc, no logging yet ===\n\n");

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

    ConnectDetach(g_coid);
    return EXIT_SUCCESS;
}
