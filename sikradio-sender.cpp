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

#define MAX_UDP_DATAGRAM_SIZE 65507

struct ControlData {
    uint16_t port;
    const char *mcast_addr;
    uint16_t data_port;
    const char *name;
};

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

void* handle_control_port(void *arg) {
    int socket_fd;
    struct sockaddr_in addr;
    char *buffer = (char *) malloc(MAX_UDP_DATAGRAM_SIZE);
    ControlData *control_data = (ControlData *) arg;
    uint16_t port = control_data->port;

    std::string msg = "BOREWICZ_HERE " + std::string(control_data->mcast_addr) + " " + 
                      std::to_string(control_data->data_port) + " " + control_data->name + "\n";

    // Create a socket
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Enable SO_REUSEADDR flag
    int reuseaddr = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) < 0) {
        perror("Failed to set SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

    // Bind the socket to a specific port
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Failed to bind socket");
        exit(EXIT_FAILURE);
    }

    while (true) {
        struct sockaddr_in sender_addr;
        socklen_t sender_addr_len = sizeof(sender_addr);
        ssize_t len = recvfrom(socket_fd, buffer, MAX_UDP_DATAGRAM_SIZE, 0,
                                (struct sockaddr *) &sender_addr, &sender_addr_len);
        if (len < 0) {
            fatal("Failed to receive data");
        }
        if (strcmp(buffer, "ZERO_SEVEN_COME_IN\n") == 0) {
            int socket_fd = socket(PF_INET, SOCK_DGRAM, 0);
            if (socket_fd < 0) {
                PRINT_ERRNO();
            }
            send_message(socket_fd, &sender_addr, msg.c_str(), msg.length());
        } else {
            // TODO: handle REXMIT
        }
    }
}

int main(int argc, char* argv[]) {
    uint64_t session_id = time(NULL);
    const char* mcast_addr = NULL;
    uint16_t data_port = 29978;
    uint16_t ctrl_port = 39978;
    size_t psize = 512;
    size_t fsize = 128 * 1024;
    size_t rtime = 250;
    std::string name = "\"Nienazwany Nadajnik\"";
    int flag;
    while((flag = getopt(argc, argv, "a:P:p:n:")) != -1) {
        switch(flag) {
        case 'a':
            mcast_addr = optarg;
            break;
        case 'P':
            data_port = read_port(optarg);
            break;
        case 'C':
            ctrl_port = read_port(optarg);
            break;
        case 'p':
            psize = atoi(optarg);
            break;
        case 'n':
            name = optarg;
            break;
        case 'f':
            fsize = atoi(optarg);
            break;
        case 'R':
            rtime = atoi(optarg);
            break;
        default:
            fatal("Unknown argument");
        }
    }
    if(mcast_addr == NULL) {
        fatal("No multicast address given");
    }
    if (psize < 1 || psize > 65535 - 16) {
        fatal("Wrong psize");
    }
    if (name.length() <= 2 || name[0] != '"' || name[name.length() - 1] != '"') {
        fatal("wrong name");
    } else {
        name = name.substr(1, name.length() - 2);
    }

    struct ControlData control_data = {ctrl_port, mcast_addr, data_port, name.c_str()};

    pthread_t control_thread;
    pthread_create(&control_thread, NULL, handle_control_port, &control_data);

    struct sockaddr_in send_address = get_send_address(mcast_addr, data_port);

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

    pthread_join(control_thread, NULL);
}