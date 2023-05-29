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
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <thread>
#include <vector>
#include <set>

#include "err.h"
#include "utils.h"

pthread_mutex_t rexmit_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
std::set<size_t> rexmit_list;
bool ended = false;
int pipe_end[2];
std::unordered_map<size_t, char *> queue;
size_t lowest_packet_number = 0;
struct sockaddr_in send_address;
size_t psize = 512;

void write_to_queue(char *packet, size_t size, size_t index) {
    queue[index] = static_cast<char *>(malloc(size));
    for (size_t i = 0; i < size; i++) {
        queue[index][i] = packet[i];
    }
}

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
            ((struct sockaddr_in *) (address_result->ai_addr))->sin_addr.s_addr;
    send_address.sin_port = htons(port); // port from the command line

    freeaddrinfo(address_result);

    return send_address;
}

void handle_control_port(uint16_t ctrl_port, const char *mcast_addr, 
                         uint16_t data_port, const char *name) {
    int socket_fd = open_udp_socket();
    set_socket_flag(socket_fd, SO_REUSEPORT);
    set_socket_flag(socket_fd, SO_REUSEADDR);
    set_socket_flag(socket_fd, SO_BROADCAST);
    set_socket_flag(socket_fd, IP_MULTICAST_TTL, IPPROTO_IP);

    std::string reply_msg = "BOREWICZ_HERE " + std::string(mcast_addr) + " " + 
                      std::to_string(data_port) + " " + name + "\n";

    bind_socket(socket_fd, ctrl_port);


    while (!ended) {
        struct sockaddr_in sender_addr;
        char *buffer = (char *) malloc(MAX_UDP_DATAGRAM_SIZE);
        if (receive_or_interrupt(socket_fd, buffer, MAX_UDP_DATAGRAM_SIZE, pipe_end[0], &sender_addr) < 0) {
            free(buffer);
            break;
        }
        std::string message(buffer);

        if (message == LOOKUP_MSG) {
            send_message(socket_fd, &sender_addr, reply_msg.c_str(), reply_msg.length());
        } else if (message.size() > 13 && message.substr(0, 13) == REXMIT_MSG) {
            std::vector<size_t> numbers = parse_rexmit_list(message.substr(13), psize);
            if (!numbers.empty()) {
                CHECK_ERRNO(pthread_mutex_lock(&rexmit_mutex));
                for (size_t i : numbers) {
                    rexmit_list.insert(i);
                }
                CHECK_ERRNO(pthread_mutex_unlock(&rexmit_mutex));
            }
        }
        free(buffer);
    }
}

void send_rexmit(int rtime, int socket_fd) {
    while (!ended) {
        CHECK_ERRNO(pthread_mutex_lock(&rexmit_mutex));
        CHECK_ERRNO(pthread_mutex_lock(&queue_mutex));
        for (size_t i : rexmit_list) {
            if (queue.find(i) != queue.end()) {
                send_message(socket_fd, &send_address, queue[i], 2 * sizeof(uint64_t) + psize);
            }
        }
        CHECK_ERRNO(pthread_mutex_unlock(&queue_mutex));
        CHECK_ERRNO(pthread_mutex_unlock(&rexmit_mutex));
        usleep(rtime * 1000);
    }
}

int main(int argc, char* argv[]) {
    uint64_t session_id = time(NULL);
    const char* mcast_addr = NULL;
    uint16_t data_port = 29978;
    uint16_t ctrl_port = 39978;
    size_t fsize = 128 * 1024;
    size_t rtime = 250;
    std::string name = "Nienazwany Nadajnik";
    int flag;
    while((flag = getopt(argc, argv, "a:P:p:n:f:R:C:")) != -1) {
        switch(flag) {
        case 'a':
            mcast_addr = optarg;
            break;
        case 'P':
            if (is_valid_number(optarg) == false) {
                fatal("Wrong data_port");
            }
            data_port = read_port(optarg);
            break;
        case 'C':
            if (is_valid_number(optarg) == false) {
                fatal("Wrong data_port");
            }
            ctrl_port = read_port(optarg);
            break;
        case 'p':
            psize = check_number(optarg);
            break;
        case 'n':
            name = optarg;
            break;
        case 'f':
            if(!is_valid_number(optarg)) {
                fatal("Wrong fsize");
            }
            fsize = strtol(optarg, NULL, 10);
            PRINT_ERRNO();
            break;
        case 'R':
            if (is_valid_number(optarg) == false) {
                fatal("Wrong rtime");
            }
            rtime = strtoul(optarg, NULL, 10);
            PRINT_ERRNO();
            break;
        default:
            fatal("Unknown argument");
        }
    }
    if (!mcast_addr || !is_valid_mcast(mcast_addr)) {
        fatal("Invalid multicast address");
    }
    if (psize < 1 || psize > 65507 - 16) {
        fatal("Wrong psize");
    }
    if (fsize == 0) {
        fatal("Wrong fsize");
    }
    if (name.length() == 0 || name.length() >= 64) {
        fatal("wrong name");
    }
    if (rtime == 0) {
        fatal("wrong rtime");
    }
    if (fsize < psize) {
        fatal("fsize < psize");
    }

    CHECK(pipe(pipe_end));

    std::thread control_thread(handle_control_port, ctrl_port, mcast_addr, 
                               data_port, name.c_str());

    send_address = get_send_address(mcast_addr, data_port);

    int socket_fd = open_udp_socket();
    set_socket_flag(socket_fd, SO_BROADCAST);

    /* join to multicast group */
    struct ip_mreq ip_mreq;
    ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(mcast_addr, &ip_mreq.imr_multiaddr) == 0) {
        fatal("inet_aton - invalid multicast address\n");
    }

    int option_value = 1;
    CHECK_ERRNO(setsockopt(socket_fd, SOL_IP, IP_MULTICAST_LOOP, &option_value,
                sizeof(option_value)));

    std::thread rexmit_thread(send_rexmit, rtime, socket_fd);

    uint64_t first_byte_num = 0;
    char *packet = static_cast<char*>(std::malloc(psize + 2 * sizeof(uint64_t)));

    session_id = htobe64(session_id);

    uint64_t first_byte_to_send;
    memcpy(packet, &session_id, sizeof(uint64_t));
    while(fread(packet + 2 * sizeof(uint64_t), 1, psize, stdin)) {
        first_byte_to_send = htobe64(first_byte_num);
        memcpy(packet + sizeof(uint64_t), &first_byte_to_send, sizeof(uint64_t));
        send_message(socket_fd, &send_address, packet, psize + 16);

        CHECK_ERRNO(pthread_mutex_lock(&queue_mutex));
        write_to_queue(packet, psize + 16, first_byte_num);
        if (queue.size() > fsize / psize) {
            free(queue[lowest_packet_number]);
            queue.erase(lowest_packet_number);
            lowest_packet_number += psize;
        }
        CHECK_ERRNO(pthread_mutex_unlock(&queue_mutex));

        first_byte_num += psize;
    }
    free(packet);

    if(write(pipe_end[1], "x", 1) < 0) {
        fatal("write");
    }
    ended = true;

    control_thread.join();
    rexmit_thread.join();

    for (auto it : queue) {
        free(it.second);
    }

    close(pipe_end[0]);
    close(pipe_end[1]);
}