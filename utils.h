#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string>

#include "err.h"

#define MAX_UDP_DATAGRAM_SIZE 65507

struct LockedData {
  pthread_mutex_t mutex;
  pthread_cond_t write;
  uint64_t last_byte_received;
  uint64_t socket_fd;
  uint64_t first_byte_in_buf;
  uint64_t byte_to_write;
  uint64_t bsize;
  uint64_t my_bsize;
  uint64_t psize;
  uint64_t session;
  char *data;
  bool *received;
  bool started_printing;
  char *src_addr;
};

struct RadioStation {
  std::string name;
  const char *mcast_addr;
  uint16_t data_port;
};

bool operator<(const struct RadioStation& a, const struct RadioStation& b) {
    return a.name < b.name;
}

bool operator==(const struct RadioStation& a, const struct RadioStation& b) {
  return a.name == b.name && strcmp(a.mcast_addr, b.mcast_addr) == 0 && a.data_port == b.data_port;
}

uint64_t max(uint64_t a, uint64_t b) {
  return a > b ? a : b;
}

void locked_data_init(struct LockedData* ld, uint64_t bsize, uint64_t psize, 
                      uint64_t socket_fd, uint64_t session, uint64_t byte_zero, 
                      const char *src_addr) {
  pthread_mutex_init(&ld->mutex, NULL);
  pthread_cond_init(&ld->write, NULL);
  ld->first_byte_in_buf = byte_zero;
  ld->byte_to_write = byte_zero;
  ld->last_byte_received = byte_zero;
  ld->psize = psize;
  ld->bsize = bsize;
  ld->my_bsize = bsize - bsize % psize; // real bsize for current session
  ld->data = static_cast<char*>(std::malloc(ld->bsize));
  ld->received = static_cast<bool*>(std::calloc(ld->my_bsize / psize, sizeof(bool)));
  ld->socket_fd = socket_fd;
  ld->session = session;
  ld->started_printing = false;
  ld->src_addr = static_cast<char*>(std::malloc(strlen(src_addr) + 1));
  strcpy(ld->src_addr, src_addr);
}

uint16_t read_port(char *string) {
    unsigned long port = strtoul(string, NULL, 10);
    PRINT_ERRNO();
    if (port > UINT16_MAX) {
        fatal("%u is not a valid port number", port);
    }

    return (uint16_t) port;
}

// int bind_socket(uint16_t port) {
//     int socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
//     ENSURE(socket_fd >= 0);
//     // after socket() call; we should close(sock) on any execution path;

//     struct sockaddr_in server_address;
//     server_address.sin_family = AF_INET; // IPv4
//     server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
//     server_address.sin_port = htons(port);

//     // bind the socket to a concrete address
//     CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &server_address,
//                         (socklen_t) sizeof(server_address)));

//     return socket_fd;
// }

size_t read_message(int socket_fd, struct sockaddr_in *client_address, void *buffer, 
                    size_t max_length, const char *expected_src_addr) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0; // we do not request anything special
    errno = 0;
    ssize_t len = 0; 

    struct addrinfo hints, *expected_ip;
    char ip_str[INET6_ADDRSTRLEN];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (expected_src_addr != NULL) {
        CHECK(getaddrinfo(expected_src_addr, NULL, &hints, &expected_ip));
        void *addr;
        if (expected_ip->ai_family == AF_INET) {
            // IPv4 address
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)expected_ip->ai_addr;
            addr = &(ipv4->sin_addr);
        } else {
            // IPv6 address
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)expected_ip->ai_addr;
            addr = &(ipv6->sin6_addr);
        }

        // Convert the IP address to a string
        inet_ntop(expected_ip->ai_family, addr, ip_str, sizeof(ip_str));
    }


    do {
        len = recvfrom(socket_fd, buffer, max_length, flags,
                      (struct sockaddr *) client_address, &address_length);
    } while (expected_src_addr != NULL && strcmp(ip_str, inet_ntoa(client_address->sin_addr)) != 0);
    if (len < 0) {
        PRINT_ERRNO();
    }
    return (size_t) len;
}

void send_message(int socket_fd, const struct sockaddr_in *send_address, 
                  const char *data, uint64_t size) {
    int send_flags = 0;
    socklen_t address_length = (socklen_t) sizeof(*send_address);
    errno = 0;
    ssize_t sent_length = sendto(socket_fd, data, size, send_flags, 
                                (struct sockaddr *) send_address, address_length);
    if (sent_length < 0) {
        PRINT_ERRNO();
    }
}

// inline static void set_port_reuse(int socket_fd) {
//     int option_value = 1;
//     CHECK_ERRNO(setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &option_value, sizeof(option_value)));
// }

inline static void set_socket_flag(int socket_fd, int flag, int level = SOL_SOCKET) {
    int option_value = 1;
    CHECK_ERRNO(setsockopt(socket_fd, level, flag, &option_value, sizeof(option_value)));
}

inline static void set_addr_reuse(int socket_fd) {
    int option_value = 1;
    CHECK_ERRNO(setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(option_value)));
}

inline static int open_udp_socket() {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        PRINT_ERRNO();
    }

    return socket_fd;
}

inline static void bind_socket(int socket_fd, uint16_t port) {
    struct sockaddr_in address;
    address.sin_family = AF_INET; // IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    address.sin_port = htons(port);

    // bind the socket to a concrete address
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &address,
                     (socklen_t) sizeof(address)));
}

inline static int open_tcp_socket() {
    int socket_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        PRINT_ERRNO();
    }

    return socket_fd;
}

inline static int accept_connection(int socket_fd, struct sockaddr_in *client_address) {
    socklen_t client_address_length = (socklen_t) sizeof(*client_address);

    int client_fd = accept(socket_fd, (struct sockaddr *) client_address, &client_address_length);
    if (client_fd < 0) {
        PRINT_ERRNO();
    }

    return client_fd;
}

inline static size_t receive_message(int socket_fd, void *buffer, size_t max_length, int flags) {
    errno = 0;
    ssize_t received_length = recv(socket_fd, buffer, max_length, flags);
    if (received_length < 0) {
        PRINT_ERRNO();
    }
    return (size_t) received_length;
}

size_t recv_with_timeout(int socket_fd, void *buffer, size_t length, int flags, int timeout) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_fd, &read_set);

    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;

    int retval = select(socket_fd + 1, &read_set, NULL, NULL, &tv);
    if (retval == -1) {
        PRINT_ERRNO();
    } else if (retval == 0) {
        return 0;
    } 
    return receive_message(socket_fd, buffer, length, flags);
}

#endif // UTILS_H