#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "utils.h"

int main(int argc, const char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int portnum = 9988;
    if (argc >= 2) {
        portnum = atoi(argv[1]);
    }
    printf("Listening on port %d\n", portnum);

    int sockfd = listen_inet_socket(portnum);
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);

    int newfd = accept(sockfd, (struct sockaddr *)&peer_addr, &peer_addr_len);
    if (newfd < 0) {
        perror("ERROR on accept");
        exit(1);
    }
    report_peer_connected(&peer_addr, peer_addr_len);

    // Set non blocking mode of socket
    int flags = fcntl(newfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        exit(1);
    }

    if (fcntl(newfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        exit(1);
    }


    while (1) {
        uint8_t buf[1024];
        printf("Calling recv...\n");
        int len = recv(newfd, buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(200 * 1000);
                continue;
            }
            perror("ERROR on recv");
            exit(1);
        } else if (len == 0) {
            printf("Peer disconnected; I'm done.\n");
            break;
        }
        printf("recv returned %d bytes\n", len);
    }
    close(newfd);
    close(sockfd);

    return 0;
}