// Threaded socket server - accepting multiple clients concurrently, by creating
// a new thread for each connecting client.
//
// Eli Bendersky [http://eli.thegreenplace.net]
// This code is in the public domain.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"
#include "tpool.h"

static const int num_threads = 4;

typedef struct { int sockfd; } thread_config_t;
typedef enum { WAIT_FOR_MSG, IN_MSG } ProcessingState;

void serve_connection(int sockfd)
{
    if (send(sockfd, "*", 1, 0) < 1) {
        perror("send");
        exit(1);
    }

    ProcessingState state = WAIT_FOR_MSG;

    while (1) {
        uint8_t buf[1024];
        int len = recv(sockfd, buf, sizeof(buf), 0);
        if (len < 0) {
            perror("recv");
            exit(1);
        } else if (len == 0) {
            break;
        }
    
        for (int i=0; i<len; ++i) {
            switch (state) {
            case WAIT_FOR_MSG:
                if (buf[i] == '^') {
                    state = IN_MSG;
                }
                break;
            case IN_MSG:
                if (buf[i] == '$') {
                    state = WAIT_FOR_MSG;
                } else {
                    buf[i] += 1;
                    if (send(sockfd, &buf[i], 1, 0) < 1) {
                        perror("send error");
                        exit(1);
                        close(sockfd);
                        return;
                    }
                }
                break;
            }
        }
    }
    close(sockfd);
}


void server_thread(void* arg) {
    thread_config_t *config = (thread_config_t *)arg;
    int sockfd = config->sockfd;
    free(config);

    // This cast will work for Linux, but in general casting pthread_id to an
    // integral type isn't portable.
    unsigned long id = (unsigned long)pthread_self();
    printf("Thread %lu created to handle connection with socket %d\n", id, sockfd);
    serve_connection(sockfd);
    printf("Thread %lu done\n", id);
    return;
}


void* wait_for_client(void* arg) {
    
    unsigned long id = (unsigned long)pthread_self();
    printf("Thread %lu created waiting for connection.\n", id);
    thread_config_t* config = (thread_config_t*)arg;
    int sockfd = config->sockfd;

    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    int newfd = accept(sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);

    id = (unsigned long)pthread_self();
    printf("Thread %lu handling connection with socket %d\n", id, newfd);

    while(1) {
        if (newfd < 0) {
            perror("ERROR on accept");
            exit(1);
        }
        config->sockfd = newfd;
        report_peer_connected(&peer_addr, peer_addr_len);
        server_thread(config);
        printf("returned from server_thread\n");
    }

    printf("Thread %lu done\n", id);
}

int main(int argc, char **argv)
{
    tpool_t *tp;

    setvbuf(stdout, NULL, _IONBF, 0);

    int portnum = 9090;
    if (argc >= 2) {
        portnum = atoi(argv[1]);
    }
    printf("Serving on port %d\n", portnum);
    fflush(stdout);

    int sockfd = listen_inet_socket(portnum);

    thread_config_t* config = (thread_config_t*)malloc(sizeof(*config));
    if (!config) {
        perror("OOM");
        exit(1);
    }
    tp = tpool_create(num_threads);
    config->sockfd = sockfd;
    for (;;) {
        struct sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        int newfd = accept(sockfd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (newfd < 0) {
            perror("ERROR on accept");
            exit(1);
        }
        report_peer_connected(&peer_addr, peer_addr_len);
        config->sockfd = newfd;
        tpool_add_work(tp, server_thread, config);
    }
    tpool_wait(tp);
    return 0;
}
