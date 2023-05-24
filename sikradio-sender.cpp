// #define _GNU_SOURCE

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <iostream>
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

// #include "err.h"
#include "utils.h"

struct sockaddr_in get_send_address(const char *host, uint16_t port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    struct addrinfo *address_result;
    CHECK(getaddrinfo(host, NULL, &hints, &address_result));

    struct sockaddr_in send_address;
    send_address.sin_family = AF_INET; // IPv4
    send_address.sin_addr.s_addr =
            ((struct sockaddr_in *) (address_result->ai_addr))->sin_addr.s_addr; // IP address
    send_address.sin_port = htons(port); // port from the command line

    freeaddrinfo(address_result);

    return send_address;
}

int main(int argc, char* argv[]) {
    uint64_t session_id = time(NULL);
    const char* dest_addr = NULL;
    uint16_t data_port = 29978;
    size_t psize = 512;
    __attribute__((unused)) const char* nazwa = "Nienazwany Nadajnik";
    int flag;
    while((flag = getopt(argc, argv, "a:P:p:n:")) != -1) {
        switch(flag) {
        case 'a':
            dest_addr = optarg;
            break;
        case 'P':
            data_port = read_port(optarg);
            break;
        case 'p':
            psize = atoi(optarg);
            break;
        case 'n':
            nazwa = optarg;
            break;
        default:
            fatal("Unknown argument");
        }
    }
    if(dest_addr == NULL) {
        fatal("No destination address given");
    }
    if (psize < 1 || psize > 65535 - 16) {
        fatal("Wrong psize");
    }
    if (nazwa == NULL || strlen(nazwa) == 0) {
        fatal("No name given");
    }


    struct sockaddr_in send_address = get_send_address(dest_addr, data_port);

    int socket_fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        PRINT_ERRNO();
    }

    uint64_t first_byte_num = 0;
    char *packet = static_cast<char*>(std::malloc(psize + 2 * sizeof(uint64_t)));

    session_id = htobe64(session_id);

    uint64_t first_byte_to_send;
    memcpy(packet, &session_id, sizeof(uint64_t));
    while(fread(packet + 2 * sizeof(uint64_t), 1, psize, stdin)) {
        first_byte_to_send = htobe64(first_byte_num);
        memcpy(packet + sizeof(uint64_t), &first_byte_to_send, sizeof(uint64_t));
        send_message(socket_fd, &send_address, packet, psize);
        first_byte_num += psize;
    }
    free(packet);
}