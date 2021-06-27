#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    printf("recv");
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
            perror("recv");
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
    printf("send");
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
            perror("send");
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
    if (client_fd >= FD_SETSIZE) {
        printf("client socket fd (%d) >= FD_SETSIZE (%d)\n", client_fd, FD_SETSIZE);
        exit(1);
    }


    fd_set readfds_master;
    FD_ZERO(&readfds_master);
    fd_set writefds_master;
    FD_ZERO(&writefds_master);

    FD_SET(client_fd, &readfds_master);
    int fdset_max = client_fd;

    while (1) {
        fd_set readfds = readfds_master;
        fd_set writefds = writefds_master;

        printf("select\n");
        int nready = select(fdset_max+1, &readfds, &writefds, NULL, NULL);
        printf("nready: %d\n", nready);

        if (nready < 0) {
            perror("select");
            exit(1);
        }

        for (int fd = 0; fd <= fdset_max && nready > 0; fd++) {

            if (FD_ISSET(fd, &readfds)) {
                nready--;

                if (fd == client_fd) {
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
                        if (newfd > fdset_max) {
                          if (newfd >= FD_SETSIZE) {
                              printf("sockfd fd (%d) >= FD_SETSIZE (%d)\n", newfd, FD_SETSIZE);
                              exit(1);
                          }
                          fdset_max = newfd;
                          printf("fdset_max set to %d\n", fdset_max);
                        }

                        fd_status_t status = on_peer_connected(newfd, &client_addr, client_addr_len);
                        if (status.want_read) {
                            FD_SET(newfd, &readfds_master);
                        } else {
                            FD_CLR(newfd, &readfds_master);
                        }
                        if (status.want_write) {
                            FD_SET(newfd, &writefds_master);
                            printf("added %d to writefds_master\n", fd);
                        } else {
                            FD_CLR(newfd, &writefds_master);
                        }

                    }
                } else {
                    fd_status_t status = on_peer_ready_recv(fd);
                    if (status.want_read) {
                        FD_SET(fd, &readfds_master);
                    } else {
                        FD_CLR(fd, &readfds_master);
                    }
                    if (status.want_write) {
                        FD_SET(fd, &writefds_master);
                    } else {
                        FD_CLR(fd, &writefds_master);
                    }
                    if (!status.want_read && !status.want_write) {
                        printf("socket %d closing\n", fd);
                        close(fd);
                    }
                }
            }

            if (FD_ISSET(fd, &writefds)) {
                    printf("fd: %d\n", fd);
                nready--;
                fd_status_t status = on_peer_ready_send(fd);
                if (status.want_read) {
                    FD_SET(fd, &readfds_master);
                } else {
                    FD_CLR(fd, &readfds_master);
                }
                if (status.want_write) {
                    FD_SET(fd, &writefds_master);
                } else {
                    FD_CLR(fd, &writefds_master);
                }
                if (!status.want_read && !status.want_write) {
                    printf("socket %d closing\n", fd);
                    close(fd);
                }
            }
        }
    }
}