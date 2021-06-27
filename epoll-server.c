#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"


#define MAXFDS 1000

typedef enum { INITIAL_ACK, WAIT_FOR_MSG, IN_MSG } ProcessingState;

#define SENDBUF_SIZE 1024

typedef struct {
    ProcessingState state;
    uint8_t sendbuf[SENDBUF_SIZE];
    int sendbuf_end;
    int sendptr;
} peer_state_t;

peer_state_t global_state[MAXFDS];

typedef struct {
    bool want_read;
    bool want_write;
} fd_status_t;

const fd_status_t fd_status_R = {.want_read = true, .want_write = false};
const fd_status_t fd_status_W = {.want_read = false, .want_write = true};
const fd_status_t fd_status_RW = {.want_read = true, .want_write = true};
const fd_status_t fd_status_NORW = {.want_read = false, .want_write = false};

fd_status_t on_peer_connected (int sockfd, const struct sockaddr_in *peer_addr, socklen_t peer_addr_len) {
    assert (sockfd < MAXFDS);
    report_peer_connected(peer_addr, peer_addr_len);

    peer_state_t *peerstate = &global_state[sockfd];
    peerstate->state = INITIAL_ACK;
    peerstate->sendbuf[0] = '*';
    peerstate->sendptr = 0;
    peerstate->sendbuf_end = 1;

    return fd_status_W;
}

fd_status_t on_peer_ready_recv(int sockfd) {
    assert(sockfd < MAXFDS);
    peer_state_t *peerstate = &global_state[sockfd];

    if (peerstate->state == INITIAL_ACK || peerstate->sendptr < peerstate->sendbuf_end) {
        return fd_status_W;
    }

    uint8_t buf[1024];
    int nbytes = recv(sockfd, buf, sizeof buf, 0);
    if (nbytes == 0) {
        return fd_status_NORW;
    } else if (nbytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return fd_status_R;
        } else {
            perror("perror recv");
            exit(1);
        }
    }
    bool ready_to_send = false;
    for (int i=0; i<nbytes; ++i) {
        switch (peerstate->state) {
        case INITIAL_ACK:
            assert(0 && "can't reach here");
            break;
        case WAIT_FOR_MSG:
            if (buf[i] == '^') {
                peerstate->state = IN_MSG;
            }
            break;
        case IN_MSG:
            if (buf[i] == '$') {
                peerstate->state = WAIT_FOR_MSG;
            } else {
                assert(peerstate->sendbuf_end < SENDBUF_SIZE);
                peerstate->sendbuf[peerstate->sendbuf_end++] = buf[i] + 1;
                ready_to_send = true;
            }
            break;
        }
    }
    return (fd_status_t){.want_read = !ready_to_send, .want_write = ready_to_send};
}


fd_status_t on_peer_ready_send(int sockfd) {
    assert(sockfd < MAXFDS);
    peer_state_t * peerstate = &global_state[sockfd];

    if (peerstate->sendptr >= peerstate->sendbuf_end) {
        return fd_status_RW;
    }
    int sendlen = peerstate->sendbuf_end - peerstate->sendptr;
    int nsent = send(sockfd, &peerstate->sendbuf[peerstate->sendptr], sendlen, 0);
    if (nsent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return fd_status_W;
        } else {
            perror("perror send");
            exit(1);
        }
    }
    if (nsent < sendlen) {
        peerstate->sendptr += nsent;
        return fd_status_W;
    } else {
        peerstate->sendptr = 0;
        peerstate->sendbuf_end = 0;

        if (peerstate->state == INITIAL_ACK) {
            peerstate->state = WAIT_FOR_MSG;
        }

        return fd_status_R;
    }
}

int main (int argc, const char ** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

        int portnum = 9090;
    if (argc >= 2) {
        portnum = atol(argv[1]);
    }
    printf("listening on port %d\n", portnum);

    int client_fd = listen_inet_socket(portnum);

    make_socket_non_blocking(client_fd);

    int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        perror("epoll_create1\n");
        exit(1);
    }

    struct epoll_event accept_event;
    accept_event.data.fd = client_fd;
    accept_event.events = EPOLLIN;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, client_fd, &accept_event) < 0) {
        perror("epoll_ctl EPOLL_CTL_ADD\n");
        exit(1);
    }

    struct epoll_event *events = calloc(MAXFDS, sizeof(struct epoll_event));
    if (events == NULL) {
        printf("Unable to allocate memory for epoll_events\n");
        exit(1);
    }

    while (1) {
        int nready = epoll_wait(epollfd, events, MAXFDS, -1);
        for (int i = 0; i <= nready; i++) {
            if (events[i].events & EPOLLERR) {
                perror("epoll_wait returned EPOLLERR\n");
                exit(1);
            }

            if (events[i].data.fd == client_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);

                int newfd = accept(client_fd, (struct sockaddr*)&client_addr, &client_addr_len );
                if (newfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        printf("accept return EAGAIN or EWOULDBLOCK");
                    } else {
                        perror("ERROR on accept");
                        exit(1);
                    }
                } else {
                    make_socket_non_blocking(newfd);
                    if (newfd >= MAXFDS) {
                        printf("socket fd (%d) >= MAXFDS (%d)\n", newfd, MAXFDS);
                    }

                    fd_status_t status = on_peer_connected(newfd, &client_addr, client_addr_len);
                    struct epoll_event event = {0};
                    event.data.fd = newfd;

                    if (status.want_read) {
                        event.events |= EPOLLIN;
                    }
                    if (status.want_write) {
                        event.events |= EPOLLOUT;
                    }
                    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, newfd, &event) < 0) {
                        perror("epoll_ctl EPOLL_CTL_ADD");
                        exit(1);
                    }
                }

            } else {
                if (events[i].events & EPOLLIN || events[i].events & EPOLLOUT) {
                    int fd = events[i].data.fd;

                    fd_status_t status;

                    if (events[i].events & EPOLLIN) {
                        status = on_peer_ready_recv(fd);
                        printf("epollin on %d\n", fd);
                    } else if (events[i].events & EPOLLOUT) {
                        status = on_peer_ready_send(fd);
                        printf("epollout on %d\n", fd);
                    }
                    struct epoll_event event = {0};
                    event.data.fd = fd;

                    if (status.want_read) {
                        event.events |= EPOLLIN;
                    }
                    if (status.want_write) {
                        event.events |= EPOLLOUT;
                    }
                    if (event.events == 0) {
                        printf("socket %d closing\n", fd);
                        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
                            perror("epoll_ctl EPOLL_CTL_DEL\n");
                            exit(1);
                        }
                        close(fd);
                    } else if (epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event) < 0) {
                        perror("epoll_ctl EPOLL_CTL_MOD");
                        exit(1);
                    }
                }
            }
        }
    }
    return 0;
}