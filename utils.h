#ifndef UTILS_H
#define UTILS_H

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

// Reports a peer connection to stdout. sa is the data populated by a successful
// accept() call.
void report_peer_connected(const struct sockaddr_in* sa, socklen_t salen);

// Creates a bound and listening INET socket on the given port number. Returns
// the socket fd when successful; dies in case of errors.
int listen_inet_socket(int portnum);

void make_socket_non_blocking(int sockfd);

#endif