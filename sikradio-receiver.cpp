#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <getopt.h>
#include <assert.h>
#include <cstdlib>
#include <stdbool.h>
#include <map>
#include <string>
#include <iostream>
#include <sstream>
#include <poll.h>
#include <sys/time.h>

#include "err.h"
#include "utils.h"

#define BROADCAST_IP "255.255.255.255"
#define TTL_VALUE 4

struct ControlData {
    uint16_t port;
    const char *addr;
    int socket_fd;
};

struct LockedData *ld;
std::map<RadioStation, uint64_t> stations;
int selected_station = 0;
pthread_mutex_t stations_mutex = PTHREAD_MUTEX_INITIALIZER;

void start_new_session(uint64_t session_id, uint64_t first_byte_num) {
    ld->session = session_id;
    ld->last_byte_received = first_byte_num;
    ld->first_byte_in_buf = first_byte_num;
    ld->byte_to_write = first_byte_num;
    ld->started_printing = false;
    ld->my_bsize = ld->bsize - ld->bsize % ld->psize;
    ld->received = static_cast<bool*>(std::realloc(ld->received, ld->my_bsize / ld->psize));
    for (uint64_t i = 0; i < ld->my_bsize / ld->psize; i++) {
        ld->received[i] = false;
    }
    for (uint64_t i = 0; i < ld->my_bsize; i++) {
        ld->data[i] = 0;
    }
}

void print_missing(uint64_t first_byte_num) {
    for (uint64_t i = ld->byte_to_write; i < first_byte_num; i += ld->psize) {
        while (i < ld->first_byte_in_buf) {
            i += ld->my_bsize;
        }
        assert(i - ld->first_byte_in_buf < ld->my_bsize);
        if (i != first_byte_num && (i >= ld->first_byte_in_buf + ld->my_bsize ||
            !ld->received[(i - ld->first_byte_in_buf) / ld->psize])) {
            fprintf(stderr, "MISSING: BEFORE %lu EXPECTED %lu\n", 
                    first_byte_num / ld->psize, i / ld->psize);
        }
    }
}

void* reader_main(__attribute__((unused)) void *arg) {
    struct sockaddr_in client_address;
    char receive_buf[65535];
    uint64_t session_id = 0;
    uint64_t first_byte_num = 0;
    while(true) {
        ld->psize = read_message(ld->socket_fd, &client_address, receive_buf, 
                                 ld->psize + 16, ld->src_addr) - 16;
        memcpy(&session_id, receive_buf, sizeof(uint64_t));
        memcpy(&first_byte_num, receive_buf + sizeof(uint64_t), sizeof(uint64_t));
        session_id = be64toh(session_id);
        first_byte_num = be64toh(first_byte_num);

        pthread_mutex_lock(&ld->mutex);
        if (session_id > ld->session) {
            start_new_session(session_id, first_byte_num);
        }

        if (session_id == ld->session && first_byte_num >= ld->first_byte_in_buf) {
            ld->last_byte_received = max(ld->last_byte_received, first_byte_num);

            if (first_byte_num >= ld->first_byte_in_buf + 3 * ld->my_bsize / 4) {
                ld->started_printing = true;
                pthread_cond_signal(&ld->write);
            }

            // circular buffer
            if (first_byte_num + ld->psize > ld->my_bsize + ld->first_byte_in_buf) {
                if (ld->first_byte_in_buf + ld->my_bsize < 
                    first_byte_num - ld->my_bsize + ld->psize) {
                    // many missing packets - save last raceived packet to the end of the buffer
                    memset(ld->received, 0, ld->my_bsize / ld->psize);
                    ld->first_byte_in_buf = first_byte_num + ld->psize - ld->my_bsize;
                } else {
                    // standard case - move buffer
                    ld->first_byte_in_buf += ld->my_bsize;
                    assert(first_byte_num >= ld->first_byte_in_buf);
                    assert(first_byte_num - ld->first_byte_in_buf < ld->my_bsize);
                    for (uint64_t i = ld->first_byte_in_buf; i < first_byte_num; i += ld->psize) {
                        ld->received[(i - ld->first_byte_in_buf) / ld->psize] = 0;
                    }
                }
            }
            
            print_missing(first_byte_num);

            // not to double writing thread
            if (first_byte_num == ld->byte_to_write + ld->bsize) {
                ld->byte_to_write += ld->psize;
            }
        
            // copy music data to buffer
            assert(first_byte_num >= ld->first_byte_in_buf);
            memcpy(ld->data + (first_byte_num - ld->first_byte_in_buf), 
                   receive_buf + 16, ld->psize);
            assert(first_byte_num >= ld->first_byte_in_buf && 
                   first_byte_num - ld->first_byte_in_buf < ld->my_bsize);
            ld->received[(first_byte_num - ld->first_byte_in_buf) / ld->psize] = true;
            if (ld->last_byte_received >= ld->byte_to_write && ld->started_printing) {
                pthread_cond_signal(&ld->write);
            }
        }
        pthread_mutex_unlock(&ld->mutex);
    }
}

void* writer_main(__attribute__((unused)) void *arg) {
    uint64_t index_to_write;
    char buf_to_print[65536];

    while(true) {
        pthread_mutex_lock(&ld->mutex);
        while (!ld->started_printing || ld->byte_to_write > ld->last_byte_received) {
            pthread_cond_wait(&ld->write, &ld->mutex);
        }
        if (ld->byte_to_write < ld->first_byte_in_buf) {
            if (ld->byte_to_write + ld->my_bsize > ld->first_byte_in_buf) {
                index_to_write = ld->byte_to_write - ld->first_byte_in_buf + ld->my_bsize;
            }
            else {
                ld->byte_to_write = ld->first_byte_in_buf;
                index_to_write = 0;
            }
        } else {
            index_to_write = ld->byte_to_write - ld->first_byte_in_buf;
        }
        assert(index_to_write % ld->psize == 0);
        // if the packet wasn't received yet, print 0s
        assert(index_to_write < ld->my_bsize);
        if (!ld->received[index_to_write / ld->psize]) {;
            for (uint64_t i = 0; i < ld->psize; i++) {
                buf_to_print[i] = 0;
            }
        }
        else {
            assert(index_to_write < ld->my_bsize);

            memcpy(buf_to_print, ld->data + index_to_write, ld->psize);

            ld->received[index_to_write / ld->psize] = false;
        }

        ld->byte_to_write += ld->psize;

        pthread_mutex_unlock(&ld->mutex);
        fwrite(buf_to_print, 1, ld->psize, stdout);
    }
    return 0;
}

void* send_lookup(void *arg) {
    // uint16_t data = *(static_cast<uint16_t*>(arg));
    struct ControlData *data = static_cast<struct ControlData*>(arg);
    // uint16_t port = data->port;
    int socket_fd = data->socket_fd;
    const char *message = "ZERO_SEVEN_COME_IN\n";
    int broadcast_permission = 1;

    // Set socket options to allow broadcast
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, (void *)&broadcast_permission,
                   sizeof(broadcast_permission)) < 0) {
        fatal("setsockopt() failed");
    }

    // Set up the destination address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(data->port);

    if (inet_aton(data->addr, &addr.sin_addr) == 0) {
        fatal("invalid discovery address");
    }
    addr.sin_port = htons(data->port);

    // Send the broadcast message and update stations
    struct timeval tp;
    while (true) {
        if (sendto(socket_fd, message, strlen(message), 0, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
            fatal("sendto() failed");
        }
        std::cout << "=========================\n";
        pthread_mutex_lock(&stations_mutex);
        gettimeofday(&tp, NULL);
        uint64_t time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
        for (const auto &it : stations) {
            // if (it.second + 5000 < time(NULL)) {
            //     stations.erase(it.first);
            // }
            std::cout << it.first.name << " " << time - it.second << "\n";
        }
        pthread_mutex_unlock(&stations_mutex);
        sleep(5);
    }
}

void* receive_reply(void *arg) {
    struct sockaddr_in sender_addr;
    int socket_fd = *(static_cast<int*>(arg));
    
    char *buf = static_cast<char*>(malloc(MAX_UDP_DATAGRAM_SIZE));

    struct timeval tp;

    while (true) {
        // size_t len = read_message(socket_fd, &sender_addr, buf, MAX_UDP_DATAGRAM_SIZE, NULL);
        size_t len = receive_message(socket_fd, buf, MAX_UDP_DATAGRAM_SIZE, 0);
        std::string message(buf, len);
        // std::cout << message;
        if (message.substr(0, 13) == "BOREWICZ_HERE") {
            gettimeofday(&tp, NULL);
            uint64_t time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
            std::istringstream ss(message.substr(13));
            std::string mcast_addr;
            uint16_t port;
            std::string name;
            ss >> mcast_addr >> port >> name;
            RadioStation station = {name, mcast_addr.c_str(), port};
            pthread_mutex_lock(&stations_mutex);
            stations[station] = time;
            pthread_mutex_unlock(&stations_mutex);
        }
    }
}

void* handle_ui(void *arg) {
    uint16_t port = *(static_cast<uint16_t*>(arg));
    int connections = 5;
    char buf[3];
    struct pollfd poll_descriptors[connections];
    for (int i = 0; i < connections; i++) {
        poll_descriptors[i].fd = -1;
        poll_descriptors[i].events = POLLIN;
        poll_descriptors[i].revents = 0;
    }
    size_t active_clients = 0;

    poll_descriptors[0].fd = open_tcp_socket();

    bind_socket2(poll_descriptors[0].fd, port);

    int queue_length = 5;
    CHECK_ERRNO(listen(poll_descriptors[0].fd, queue_length));

    do {
        for (int i = 0; i < connections; i++) {
            poll_descriptors[i].revents = 0;
        }

        int poll_status = poll(poll_descriptors, connections, -1);
        if (poll_status < 0) {
            fatal("poll() failed");
        }
        else if (poll_status == 0) {
            fatal("poll() timed out");
        }
        else {
            if (poll_descriptors[0].revents & POLLIN) {
                int client_fd = accept_connection(poll_descriptors[0].fd, NULL);
                bool accepted = false;
                for (int i = 1; i < connections; ++i) {
                    if (poll_descriptors[i].fd == -1) {
                        poll_descriptors[i].fd = client_fd;
                        poll_descriptors[i].events = POLLIN;
                        active_clients++;
                        accepted = true;
                        break;
                    }
                }
                if (!accepted) {
                    CHECK_ERRNO(close(client_fd));
                    fprintf(stderr, "Too many clients\n");
                }
            }
            for (int i = 1; i < connections; i++) {
                if (poll_descriptors[i].fd != -1 && poll_descriptors[i].revents & (POLLIN | POLLERR)) {
                    ssize_t received_bytes = read(poll_descriptors[i].fd, buf, 3);
                    if (received_bytes < 0) {
                        fprintf(stderr, "Error when reading message from connection %d (errno %d, %s)\n", i, errno, strerror(errno));
                        CHECK_ERRNO(close(poll_descriptors[i].fd));
                        poll_descriptors[i].fd = -1;
                        active_clients -= 1;
                    } else if (received_bytes == 0) {
                        fprintf(stderr, "Ending connection (%d)\n", i);
                        CHECK_ERRNO(close(poll_descriptors[i].fd));
                        poll_descriptors[i].fd = -1;
                        active_clients -= 1;
                    } else {
                        std::string message(buf, 3);
                        std::cout << message;
                    }
                }
            }
        }
    } while (true);
}

int main(int argc, char *argv[]) {
    uint16_t data_port = 29956;
    size_t bsize = 655368;
    uint16_t control_port = 39956;
    const char* src_addr = NULL;
    const char* discover_addr = BROADCAST_IP;
    uint16_t ui_port = 19956;
    int flag;
    while((flag = getopt(argc, argv, "a:P:b:C:d:U")) != -1) {
        switch(flag) {
            case 'a':
                src_addr = optarg;
                break;
            case 'P':
                data_port = read_port(optarg);
                break;
            case 'b':
                bsize = atoi(optarg);
                break;
            case 'C':
                control_port = read_port(optarg);
                break;
            case 'd':
                discover_addr = optarg;
                break;
            case 'U':
                ui_port = read_port(optarg);
                break;
            default:
                fatal("Wrong flag");
        }
    }
    if(src_addr == NULL) {
        fatal("No source address given");
    }
    if (bsize < 1) {
        fatal("Wrong buffer size");
    }

    struct ControlData *ctrl_data = static_cast<struct ControlData*>(std::malloc(sizeof(struct ControlData)));
    ctrl_data->addr = discover_addr;

    int socket_fd = open_udp_socket();

    /* uaktywnienie rozgłaszania (ang. broadcast) */
    int optval = 1;
    CHECK_ERRNO(setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, (void *) &optval, sizeof optval));

    // /* ustawienie TTL dla datagramów rozsyłanych do grupy */
    // optval = TTL_VALUE;
    // CHECK_ERRNO(setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, (void *) &optval, sizeof optval));

    set_port_reuse(socket_fd);
    set_addr_reuse(socket_fd);

    // /* ustawienie adresu i portu odbiorcy */
    // struct sockaddr_in remote_address;
    // remote_address.sin_family = AF_INET;
    // remote_address.sin_port = htons(control_port);
    // if (inet_aton(discover_addr, &remote_address.sin_addr) == 0) {
    //     fprintf(stderr, "ERROR: inet_aton - invalid multicast address\n");
    //     exit(EXIT_FAILURE);
    // }

    // int broadcast_permission = 1;
    // // Set socket options to allow broadcast
    // if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, (void *)&broadcast_permission,
    //                sizeof(broadcast_permission)) < 0) {
    //     fatal("setsockopt() failed");
    // }

    // int optval = 4;
    // CHECK_ERRNO(setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, (void *) &optval, sizeof(optval)));

    // bind_socket2(socket_fd, control_port);
    ctrl_data->socket_fd = socket_fd;
    ctrl_data->port = control_port;

    pthread_t send_lookup_thread;
    pthread_t receive_reply_thread;
    pthread_t ui_thread;

    pthread_create(&send_lookup_thread, NULL, send_lookup, static_cast<void*>(ctrl_data));
    pthread_create(&receive_reply_thread, NULL, receive_reply, static_cast<void*>(&socket_fd));
    // pthread_create(&ui_thread, NULL, handle_ui, static_cast<void*>(&ui_port));

    // char *receive_buf = static_cast<char*>(std::malloc(65536 + 16));

    // // int socket_fd = bind_socket(data_port);
    // int socket_fd = open_udp_socket();
    // set_port_reuse(socket_fd);
    // // bind_socket2(socket_fd, data_port);

    // size_t psize;
    // struct sockaddr_in client_address;
    // psize = read_message(socket_fd, &client_address, receive_buf, 65535, src_addr) - 16;
    // uint64_t session_id;
    // uint64_t byte_zero;
    // memcpy(&session_id, receive_buf, sizeof(uint64_t));
    // memcpy(&byte_zero, receive_buf + sizeof(uint64_t), sizeof(uint64_t));

    // session_id = be64toh(session_id);
    // byte_zero = be64toh(byte_zero);

    // ld = static_cast<struct LockedData*>(std::malloc(sizeof(struct LockedData)));
    // locked_data_init(ld, bsize, psize, socket_fd, session_id, byte_zero, src_addr);
    // memcpy(ld->data, receive_buf + 2 * sizeof(uint64_t), psize);
    // ld->received[0] = true;

    // pthread_t writer_thread;
    // pthread_t reader_thread;


    // pthread_create(&writer_thread, NULL, writer_main, NULL);
    // pthread_create(&reader_thread, NULL, reader_main, NULL);


    // pthread_join(reader_thread, NULL);
    // pthread_join(writer_thread, NULL);
    pthread_join(send_lookup_thread, NULL);
    pthread_join(receive_reply_thread, NULL);
    return 0;
}