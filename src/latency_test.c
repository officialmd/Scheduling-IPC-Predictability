/*
latency_test.c

latency vs message size experiment

this measures round-trip ipc latency for different message sizes
to replicate figure 6

what this does:
  - one client thread and one server thread on the same machine
  - the client sends messages of increasing size (96 to 2048 bytes)
  - for each size, we run many iterations and record the round-trip
    time from just before MsgSend to just after it returns
  - we report min, avg, and max latency for each message size
  - no aps, no inheritance, just raw ipc timing

the paper's figure 6 shows that round-trip latency increases roughly
linearly with message size. our results should show the same trend
even if the absolute values differ due to hardware differences.

message sizes tested: 96, 128, 256, 512, 1024, 1536, 2048 bytes
iterations per size: 10000
server priority: 238, client priority: 236 (same offset as setting a)
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
#include <stdint.h>
#include <float.h>

#define SERVER_PRIORITY 238
#define CLIENT_PRIORITY 236
#define ITERATIONS      10000
#define MAX_MSG_SIZE    2048
#define NS_TO_US(ns)    ((double)(ns) / 1000.0)

// message sizes to test, matching the paper's range
static const int msg_sizes[] = { 96, 128, 256, 512, 1024, 1536, 2048 };
#define NUM_SIZES (sizeof(msg_sizes) / sizeof(msg_sizes[0]))

static volatile int g_chid = -1;
static volatile int g_coid = -1;
static volatile int g_running = 1;

// buffers big enough for the largest message
static char g_send_buf[MAX_MSG_SIZE];
static char g_recv_buf[MAX_MSG_SIZE];

typedef struct {
    double min_us;
    double max_us;
    double avg_us;
    int    msg_size;
    int    iterations;
} latency_result_t;

static latency_result_t g_results[NUM_SIZES];

/*
input: none
output: none
description: server thread that receives messages and replies immediately.
the reply is the same size as the received message so that both
directions carry the full payload. this matches how the paper
measures round-trip communication latency.
*/
static void *server_thread(void *arg) {
    (void)arg;

    // no inheritance, fixed priority
    g_chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY);
    if (g_chid == -1) {
        perror("ChannelCreate failed");
        exit(EXIT_FAILURE);
    }

    printf("[server] started at priority %d, channel %d\n",
           SERVER_PRIORITY, g_chid);

    char buf[MAX_MSG_SIZE];
    struct _msg_info info;

    while (g_running) {
        int rcvid = MsgReceive(g_chid, buf, sizeof(buf), &info);
        if (rcvid == -1) {
            if (!g_running) break;
            perror("MsgReceive error");
            break;
        }

        // reply with the same size message
        // info.msglen tells us how many bytes the client sent
        MsgReply(rcvid, EOK, buf, info.msglen);
    }

    ChannelDestroy(g_chid);
    return NULL;
}

/*
input: none
output: none
description: client thread that runs the latency benchmark.
for each message size, it sends ITERATIONS messages and measures
the round-trip time using CLOCK_MONOTONIC. records min, avg,
and max for each size.
*/
static void *client_thread(void *arg) {
    (void)arg;

    // wait for server to be ready
    while (g_coid == -1) usleep(1000);

    printf("[client] started at priority %d\n", CLIENT_PRIORITY);
    printf("[client] running %d iterations per message size\n\n", ITERATIONS);

    // fill send buffer with some data so it's not all zeros
    for (int i = 0; i < MAX_MSG_SIZE; i++) {
        g_send_buf[i] = (char)(i & 0xFF);
    }

    // warm up: do a few sends to get things loaded
    for (int w = 0; w < 100; w++) {
        MsgSend(g_coid, g_send_buf, 96, g_recv_buf, 96);
    }

    // run the benchmark for each message size
    for (int s = 0; s < (int)NUM_SIZES; s++) {
        int size = msg_sizes[s];
        double min_us = DBL_MAX;
        double max_us = 0.0;
        double total_us = 0.0;

        printf("[client] testing %d bytes...\n", size);

        for (int i = 0; i < ITERATIONS; i++) {
            struct timespec t_start, t_end;

            clock_gettime(CLOCK_MONOTONIC, &t_start);
            int ret = MsgSend(g_coid, g_send_buf, size, g_recv_buf, size);
            clock_gettime(CLOCK_MONOTONIC, &t_end);

            if (ret == -1) {
                perror("MsgSend error");
                g_running = 0;
                return NULL;
            }

            // compute round-trip time in microseconds
            long long ns = (long long)(t_end.tv_sec - t_start.tv_sec) * 1000000000LL
                         + (long long)(t_end.tv_nsec - t_start.tv_nsec);
            double us = NS_TO_US(ns);

            if (us < min_us) min_us = us;
            if (us > max_us) max_us = us;
            total_us += us;
        }

        g_results[s].msg_size   = size;
        g_results[s].iterations = ITERATIONS;
        g_results[s].min_us     = min_us;
        g_results[s].max_us     = max_us;
        g_results[s].avg_us     = total_us / ITERATIONS;
    }

    g_running = 0;
    return NULL;
}

static void print_results(void) {
    printf("\n========== latency vs message size ==========\n");
    printf("%-10s %-12s %-12s %-12s %-10s\n",
           "size(B)", "min(us)", "avg(us)", "max(us)", "iters");
    printf("%-10s %-12s %-12s %-12s %-10s\n",
           "-------", "-------", "-------", "-------", "-----");

    for (int i = 0; i < (int)NUM_SIZES; i++) {
        printf("%-10d %-12.2f %-12.2f %-12.2f %-10d\n",
               g_results[i].msg_size,
               g_results[i].min_us,
               g_results[i].avg_us,
               g_results[i].max_us,
               g_results[i].iterations);
    }
    printf("=============================================\n");

    printf("\ncompare against the paper's figure 6.\n");
    printf("things to check:\n");
    printf("  - latency should increase roughly linearly with message size\n");
    printf("  - the trend shape matters more than absolute values\n");
    printf("    (the paper uses hardware, so there is more overhead with our virtualization)\n");
    printf("  - min latency shows the best-case communication overhead\n");
    printf("  - max latency shows worst-case jitter\n");
}

int main(void) {
    printf("=== latency vs message size experiment (figure 6) ===\n");
    printf("message sizes: ");
    for (int i = 0; i < (int)NUM_SIZES; i++) {
        printf("%d%s", msg_sizes[i], i < (int)NUM_SIZES - 1 ? ", " : "");
    }
    printf(" bytes\n");
    printf("iterations per size: %d\n\n", ITERATIONS);

    // start server thread
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
        return EXIT_FAILURE;
    }
    pthread_attr_destroy(&server_attr);

    // wait for channel
    while (g_chid == -1) usleep(1000);

    g_coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_coid == -1) {
        perror("ConnectAttach failed");
        return EXIT_FAILURE;
    }
    printf("[main] connected to server (chid=%d, coid=%d)\n\n", g_chid, g_coid);

    // start client thread
    pthread_t          client_tid;
    pthread_attr_t     client_attr;
    struct sched_param client_sp;

    pthread_attr_init(&client_attr);
    pthread_attr_setinheritsched(&client_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&client_attr, SCHED_FIFO);
    client_sp.sched_priority = CLIENT_PRIORITY;
    pthread_attr_setschedparam(&client_attr, &client_sp);

    ret = pthread_create(&client_tid, &client_attr, client_thread, NULL);
    if (ret != 0) {
        fprintf(stderr, "failed to create client thread: %s\n", strerror(ret));
        return EXIT_FAILURE;
    }
    pthread_attr_destroy(&client_attr);

    // wait for client to finish
    pthread_join(client_tid, NULL);

    // stop server
    g_running = 0;
    // send a dummy message to unblock the server from MsgReceive
    char dummy = 0;
    MsgSend(g_coid, &dummy, 1, &dummy, 1);

    pthread_join(server_tid, NULL);

    ConnectDetach(g_coid);

    print_results();

    return EXIT_SUCCESS;
}
