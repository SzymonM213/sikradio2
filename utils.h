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

#include "err.h"

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

int bind_socket(uint16_t port) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
    ENSURE(socket_fd >= 0);
    // after socket() call; we should close(sock) on any execution path;

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET; // IPv4
    server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    server_address.sin_port = htons(port);

    // bind the socket to a concrete address
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &server_address,
                        (socklen_t) sizeof(server_address)));

    return socket_fd;
}

size_t read_message(int socket_fd, struct sockaddr_in *client_address, void *buffer, 
                    size_t max_length, const char *expected_src_addr) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0; // we do not request anything special
    errno = 0;
    ssize_t len = 0; 
    do {
        len = recvfrom(socket_fd, buffer, max_length, flags,
                      (struct sockaddr *) client_address, &address_length);
    } while (strcmp(inet_ntoa(client_address->sin_addr), expected_src_addr) != 0 && 
             strcmp(expected_src_addr, "localhost") != 0 && 
             strcmp(inet_ntoa(client_address->sin_addr),"127.0.0.1") != 0);
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
    ssize_t sent_length = sendto(socket_fd, data, size + 16, send_flags, 
                                (struct sockaddr *) send_address, address_length);
    if (sent_length < 0) {
        PRINT_ERRNO();
    }
}

#endif // UTILS_H