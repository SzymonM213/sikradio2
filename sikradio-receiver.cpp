#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <stdio.h>
// #include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <getopt.h>
#include <assert.h>
#include <cstdlib>
#include <stdbool.h>
#include <map>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <poll.h>
#include <sys/time.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <thread>
#include <regex>

#include "err.h"
#include "utils.h"

#define BROADCAST_IP "255.255.255.255"
#define TTL_VALUE 4

size_t bsize;
uint16_t data_port;
std::string fav_name = "";
int pipe_fd[2];
int reader_pipe_fd[2];

constexpr std::string_view UI_HEADER =
"------------------------------------------------------------------------\n\r\n\r"
"SIK Radio\n\r"
"------------------------------------------------------------------------\n\r\n\r";

constexpr std::string_view UI_FOOTER =
"------------------------------------------------------------------------\n\r\n\r";

struct ControlData {
    uint16_t port;
    const char *addr;
    int socket_fd;
};

struct LockedData *ld;
std::map<RadioStation, uint64_t> stations;
std::map<RadioStation, uint64_t>::iterator selected_station;
pthread_mutex_t stations_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t no_stations = PTHREAD_COND_INITIALIZER;

void move_selected_station(bool up) {
    std::cerr << "move_selected_station" << std::endl;
    if (stations.size() < 2) {
        std::cerr << "only one station\n" << std::endl;
        if(write(pipe_fd[1], "1", 1) < 0)  {
            fatal("write");
        }
        if(write(reader_pipe_fd[1], "1", 1) < 0) {
            fatal("write");
        }
        ld->selected.store(false);
        return;
    }
    if(!up) {
        // CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
        selected_station++;
        if (selected_station == stations.end()) {
            selected_station = stations.begin();
        } 

        // CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
    }
    else {
        // CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
        if (selected_station == stations.begin()) {
            selected_station = stations.end();
        } 
        selected_station--;

        // CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
    }
    if(write(pipe_fd[1], "1", 1) < 0)  {
        fatal("write");
    }
    if(write(reader_pipe_fd[1], "1", 1) < 0) {
        fatal("write");
    }
    ld->selected.store(false);
}

void remove_station(std::map<RadioStation, uint64_t>::iterator station) {
    std::cerr << "removing station: " << station->first.name << std::endl;
    if (station == selected_station) {
        std::cerr << "looking for " << fav_name << std::endl;
        for (auto it = stations.begin(); it != stations.end(); it++) {
            if (it->first.name == fav_name) {
                selected_station = it;
                // if(write(pipe_fd[1], "1", 1) < 0)  {
                //     fatal("write");
                // }
                if(write(reader_pipe_fd[1], "1", 1) < 0) {
                    fatal("write");
                }
                return;
            }
        }
        std::cerr << "no fav station" << std::endl;
        // CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
        move_selected_station(false);
        // CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
        // printf("chuj2\n");
    }
    stations.erase(station);
}

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

size_t find_waiting_time() {
    size_t waiting_time = 20000;
    struct timeval tp;
    for (auto it = stations.begin(); it != stations.end(); it++) {
        gettimeofday(&tp, NULL);
        uint64_t time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
        if (time - it->second < waiting_time) {
            waiting_time = time - it->second;
        }
    }
    return waiting_time;
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

void reader_main(const char *src_addr, uint16_t data_port, size_t bsize) {
    std::cerr << "new addr: " << src_addr << std::endl;
    char *receive_buf = static_cast<char*>(std::malloc(65536 + 16));

    int data_socket_fd = open_udp_socket();
    set_socket_flag(data_socket_fd, SO_REUSEPORT);

    /* podłączenie do grupy rozsyłania (ang. multicast) */
    struct ip_mreq ip_mreq;
    ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(src_addr, &ip_mreq.imr_multiaddr) == 0) {
        fatal("inet_aton - invalid multicast address\n");
    }

    CHECK_ERRNO(setsockopt(data_socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void *) &ip_mreq, sizeof ip_mreq));

    bind_socket(data_socket_fd, data_port);

    ssize_t psize;
    std::cerr << "chuj\n";
    psize = receive_or_interrupt(data_socket_fd, receive_buf, MAX_UDP_DATAGRAM_SIZE, reader_pipe_fd[0]) - 16;
    std::cerr << "chuj2\n";
    if (psize < 0) {
        ld->selected.store(false);
        CHECK_ERRNO(pthread_mutex_lock(&ld->mutex));
        pthread_cond_signal(&ld->write);
        CHECK_ERRNO(pthread_mutex_unlock(&ld->mutex));
        CHECK_ERRNO(close(ld->socket_fd));
        std::cerr << "koniec readera2\n";
        return;
    }
    uint64_t session_id = 0;
    uint64_t byte_zero;
    memcpy(&session_id, receive_buf, sizeof(uint64_t));
    memcpy(&byte_zero, receive_buf + sizeof(uint64_t), sizeof(uint64_t));

    session_id = be64toh(session_id);
    byte_zero = be64toh(byte_zero);



    std::cerr << "lock2\n";
    CHECK_ERRNO(pthread_mutex_lock(&ld->mutex));
    locked_data_set(ld, bsize, psize, data_socket_fd, session_id, byte_zero, src_addr);
    std::cerr << "polock2\n";
    // CHECK_ERRNO(pthread_mutex_lock(&ld->mutex));
    memcpy(ld->data, receive_buf + 2 * sizeof(uint64_t), psize);
    ld->received[0] = true;
    CHECK_ERRNO(pthread_mutex_unlock(&ld->mutex));
    uint64_t first_byte_num = 0;
    while(ld->selected) {
        std::cerr << "chuj3\n";
        if (receive_or_interrupt(ld->socket_fd, receive_buf, ld->psize + 16, reader_pipe_fd[0]) < 0) {
            std::cerr << "chuj3.5\n";
            break;
        }
        std::cerr << "chuj4\n";
        memcpy(&session_id, receive_buf, sizeof(uint64_t));
        memcpy(&first_byte_num, receive_buf + sizeof(uint64_t), sizeof(uint64_t));
        session_id = be64toh(session_id);
        first_byte_num = be64toh(first_byte_num);

        std::cerr << "lock1\n";
        CHECK_ERRNO(pthread_mutex_lock(&ld->mutex));
        std::cerr << "polock1\n";
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
            std::cerr << "first_byte_num: " << first_byte_num << "first_byte_in_buf: " << ld->first_byte_in_buf << "\n";
            std::cerr << "my_bsize: " << ld->my_bsize << "\n";
            assert(first_byte_num >= ld->first_byte_in_buf && 
                   first_byte_num - ld->first_byte_in_buf < ld->my_bsize);
            ld->received[(first_byte_num - ld->first_byte_in_buf) / ld->psize] = true;
            if (ld->last_byte_received >= ld->byte_to_write && ld->started_printing) {
                // std::cerr << "podnoszę\n";
                pthread_cond_signal(&ld->write);
            }
        }
        CHECK_ERRNO(pthread_mutex_unlock(&ld->mutex));
    }
    ld->selected.store(false);
    CHECK_ERRNO(pthread_mutex_lock(&ld->mutex));
    pthread_cond_signal(&ld->write);
    CHECK_ERRNO(pthread_mutex_unlock(&ld->mutex));
    CHECK_ERRNO(close(ld->socket_fd));
    std::cerr << "koniec readera\n";
}

void writer_main() {
    // std::cerr << "writer started" << std::endl;
    uint64_t index_to_write;
    char buf_to_print[65536];

    while(ld->selected) {
        CHECK_ERRNO(pthread_mutex_lock(&ld->mutex));
        while (!ld->started_printing || ld->byte_to_write > ld->last_byte_received) {
            std::cerr << "writer sie wiesza\n";
            pthread_cond_wait(&ld->write, &ld->mutex);
            std::cerr << "writer sie nie wiesza\n";
            if (!ld->selected) {
                CHECK_ERRNO(pthread_mutex_unlock(&ld->mutex));
                std::cerr << "koniec writera2\n";
                return;
            }
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
        std::cerr << "index_to_write: " << index_to_write << "my_bsize: " << ld->my_bsize << std::endl;
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

        CHECK_ERRNO(pthread_mutex_unlock(&ld->mutex));
        fwrite(buf_to_print, 1, ld->psize, stdout);
        // for (size_t i = 0; i < ld->psize; i++) {
        //     printf("%c", buf_to_print[i]);
        // }
    }
    std::cerr << "koniec writera\n";
}

void send_lookup(uint16_t port, const char *addr, int socket_fd) {
    // uint16_t data = *(static_cast<uint16_t*>(arg));
    // struct ControlData *data = static_cast<struct ControlData*>(arg);
    // uint16_t port = data->port;
    // int socket_fd = data->socket_fd;
    const char *message = "ZERO_SEVEN_COME_IN\n";
    int broadcast_permission = 1;

    // Set socket options to allow broadcast
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, (void *)&broadcast_permission,
                   sizeof(broadcast_permission)) < 0) {
        fatal("setsockopt() failed");
    }

    // Set up the destination address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    if (inet_aton(addr, &dest_addr.sin_addr) == 0) {
        fatal("invalid discovery address");
    }
    dest_addr.sin_port = htons(port);

    // Send the broadcast message and update stations
    struct timeval tp;
    while (true) {
        // std::cerr << message;
        if (sendto(socket_fd, message, strlen(message), 0, (struct sockaddr *) &dest_addr, sizeof(dest_addr)) < 0) {
            fatal("sendto() failed");
        }
        std::cerr << "=========================\n";
        CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
        gettimeofday(&tp, NULL);
        uint64_t time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
        for (const auto &it : stations) {
            std::cerr << it.first.name << " " << time - it.second << "\n";
        }
        CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
        sleep(5);
    }
}

void receive_reply(int socket_fd) {    
    char *buf = static_cast<char*>(malloc(MAX_UDP_DATAGRAM_SIZE));
    memset(buf, 0, MAX_UDP_DATAGRAM_SIZE);

    struct timeval tp;

    while (true) {
        std::string message = receive_string(socket_fd, MAX_UDP_DATAGRAM_SIZE);
        std::cerr << "received: " << message;
        std::smatch matches;
        if (std::regex_match(message, matches, REPLY_REGEX)) {
            gettimeofday(&tp, NULL);
            uint64_t time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
            // std::istringstream ss(message.substr(13));
            // std::string mcast_addr;
            // uint16_t port;
            // std::string name;
            // ss >> mcast_addr >> port >> name;
            pthread_mutex_lock(&stations_mutex);
            std::cerr << "DODAJĘ STACJĘ\n";
            for (auto it = stations.begin(); it != stations.end(); it++) {
                std::cerr << it->first.name << " " << it->first.mcast_addr << " " << it->first.data_port << "\n";
            }

            std::string mcast_addr = matches[1].str();

            std::cerr << "NOWA STACJA\n";
            //iterate over stations
            for (auto it = stations.begin(); it != stations.end(); it++) {
                std::cerr << it->first.name << " " << it->first.mcast_addr << " " << it->first.data_port << "\n";
            }
            pthread_mutex_unlock(&stations_mutex);
            std::cerr << "NOWY ADRES: " << mcast_addr << "\n";
            size_t port = std::stoul(matches[2]);
            std::string name = matches[3];
            if (port > UINT16_MAX || port == 0) {
                continue;
            }
            RadioStation station = {name, mcast_addr.c_str(), (uint16_t) port};
            CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
            if (stations.find(station) != stations.end()) {
                stations[station] = time;
            }
            else {
                stations.insert({station, time});
                if(write(pipe_fd[1], "a", 1) < 0) {
                    fatal("write() failed");
                }
            }
            // stations[station] = time;
            if (stations.size() == 1) {
                selected_station = stations.begin();
                CHECK_ERRNO(pthread_cond_signal(&no_stations));
                // std::thread writer_thread(writer_main);
                // std::thread reader_thread(reader_main, station.mcast_addr, data_port, bsize);

            }
            CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
        }

        // CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
        // for (auto it = stations.begin(); it != stations.end();) {
        //     gettimeofday(&tp, NULL);
        //     uint64_t time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
        //     if (it->second + 20000 < time) {
        //         printf("PAPA SENDER\n");
        //         remove_station(it++);
        //         printf("PAPA SENDER2\n");
        //         write(pipe_fd[1], "r", 1);
        //     } else {
        //         it++;
        //     }
        // }
        // CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
    }
}

std::string make_ui() {
    std::string result = "\033[2J\033[H";
    result += UI_HEADER;
    CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
    for (auto it = stations.begin(); it != stations.end(); it++) {
        if (it == selected_station) {
            result += " > ";
        }
        result += it->first.name + "\n\r\n\r";
    }
    CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
    result += UI_FOOTER;
    return result;
}

void handle_ui(uint16_t port) {
    // uint16_t port = *(static_cast<uint16_t*>(arg));
    int connections = 5;
    int buf_size = 4;

    // char buf[connections][buf_size];
    std::vector<char *> buf(connections);

    std::vector<ssize_t> buf_len(connections);
    std::vector<ssize_t> buf_pos(connections);

    // struct pollfd poll_descriptors[connections];
    std::vector<struct pollfd> poll_descriptors(connections);
    for (int i = 0; i < connections; i++) {
        poll_descriptors[i].fd = -1;
        poll_descriptors[i].events = POLLIN;
        poll_descriptors[i].revents = 0;
        buf[i] = static_cast<char*>(malloc(buf_size));
    }
    size_t active_clients = 0;

    poll_descriptors[0].fd = open_tcp_socket();
    poll_descriptors[1].fd = pipe_fd[0];

    set_socket_flag(poll_descriptors[0].fd, SO_REUSEPORT);

    bind_socket(poll_descriptors[0].fd, port);

    int queue_length = 5;
    CHECK_ERRNO(listen(poll_descriptors[0].fd, queue_length));

    while (true) {
        for (int i = 0; i < connections; i++) {
            poll_descriptors[i].revents = 0;
        }

        size_t timeout = find_waiting_time();
        int poll_status = poll(poll_descriptors.data(), connections, timeout);
        if (poll_status == -1) {
            fatal("poll() failed");
        }
        else {
            if (poll_descriptors[0].revents & POLLIN) {
                int client_fd = accept_connection(poll_descriptors[0].fd, NULL);

                CHECK_ERRNO(fcntl(client_fd, F_SETFL, O_NONBLOCK)); /* tryb nieblokujący */

                bool accepted = false;
                int client_id;
                for (int i = 2; i < connections; ++i) {
                    if (poll_descriptors[i].fd == -1) {
                        poll_descriptors[i].fd = client_fd;
                        poll_descriptors[i].events = POLLIN;
                        active_clients++;
                        client_id = i;
                        accepted = true;
                        break;
                    }
                }
                if (!accepted) {
                    poll_descriptors.push_back(pollfd());
                    poll_descriptors[connections].fd = client_fd;
                    poll_descriptors[connections].events = POLLIN;
                    // buf[connections] = static_cast<char*>(malloc(buf_size));
                    buf.push_back(static_cast<char*>(malloc(buf_size)));
                    buf_len.push_back(0);
                    buf_pos.push_back(0);
                    connections++;
                    active_clients++;
                    client_id = connections;
                }
                if (write(client_fd, "\377\375\042\377\373\001",6) < 0) {
                    CHECK_ERRNO(close(client_fd));
                    poll_descriptors[client_id].fd = -1;
                    active_clients -= 1;
                };
            }
            if (poll_descriptors[1].revents & POLLIN) {
                std::cerr << "refresh\n";
                ssize_t received_bytes = read(poll_descriptors[1].fd, buf[1], buf_size);
                if (received_bytes < 0) {
                    fatal("read() failed");
                }
                std::string msg = make_ui();
                for (int i = 2; i < connections; i++) {
                    if (poll_descriptors[i].fd != -1) {
                        ssize_t sent_bytes = write(poll_descriptors[i].fd, msg.c_str(), msg.size());
                        if (sent_bytes < 0) {
                            fprintf(stderr, "Error when sending message to connection %d (errno %d, %s)\n", i, errno, strerror(errno));
                            CHECK_ERRNO(close(poll_descriptors[i].fd));
                            poll_descriptors[i].fd = -1;
                            active_clients -= 1;
                        }
                    }
                }
            }
            for (int i = 2; i < connections; i++) {
                if (poll_descriptors[i].fd != -1 && (poll_descriptors[i].revents & (POLLIN | POLLERR))) {
                    ssize_t received_bytes = read(poll_descriptors[i].fd, buf[i], buf_size);
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
                        std::string message(buf[i], 3);
                        buf_len[i] = received_bytes;
                        buf_pos[i] = 0;
                        if(message == "\033[B") {
                            CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
                            move_selected_station(0);
                            CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
                        }
                        // check up arrow
                        else if(message == "\033[A") {
                            CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
                            move_selected_station(1);
                            CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
                        }
                    }
                }
                std::string msg = make_ui();
                for (int j = 2; j < connections; j++) {
                    if (poll_descriptors[j].fd != -1) {
                        if(write(poll_descriptors[j].fd, msg.c_str(), msg.size()) < 0) {
                            CHECK_ERRNO(close(poll_descriptors[j].fd));
                            poll_descriptors[j].fd = -1;
                            active_clients -= 1;
                        }
                    }
                }
            }   
        }
        struct timeval tp;
        CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
        for (auto it = stations.begin(); it != stations.end();) {
            gettimeofday(&tp, NULL);
            uint64_t time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
            if (it->second + 20000 < time) {
                remove_station(it++);
                if(write(pipe_fd[1], "r", 1) < 0) {
                    fatal("write() failed");
                }
            } else {
                it++;
            }
        }
        CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
    } 
}

int main(int argc, char *argv[]) {
    // uint16_t data_port = 29978;
    // size_t bsize = 655368;
    // data_port = 29978;
    bsize = 655368;
    uint16_t control_port = 39978;
    const char* discover_addr = BROADCAST_IP;
    uint16_t ui_port = 19978;
    int flag;
    size_t rtime = 250;
    while((flag = getopt(argc, argv, "b:C:d:U:n:R:")) != -1) {
        switch(flag) {
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
            case 'n':
                fav_name = std::string(optarg);
                break;
            case 'R':
                rtime = atoi(optarg);
                break;
            default:
                fatal("Wrong flag");
        }
    }
    if (bsize < 1) {
        fatal("Wrong buffer size");
    }

    int socket_fd = open_udp_socket();
    set_socket_flag(socket_fd, SO_BROADCAST);
    set_socket_flag(socket_fd, SO_REUSEPORT);

    CHECK(pipe(pipe_fd));
    CHECK(pipe(reader_pipe_fd));

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
    // ctrl_data->socket_fd = socket_fd;
    // ctrl_data->port = control_port;

    // pthread_t send_lookup_thread;
    // pthread_t receive_reply_thread;
    // pthread_t ui_thread;

    // pthread_create(&send_lookup_thread, NULL, send_lookup, static_cast<void*>(ctrl_data));
    // pthread_create(&receive_reply_thread, NULL, receive_reply, static_cast<void*>(&socket_fd));
    // pthread_create(&ui_thread, NULL, handle_ui, static_cast<void*>(&ui_port));
    std::thread ui_thread(handle_ui, ui_port);
    std::thread send_lookup_thread(send_lookup, control_port, discover_addr, socket_fd);
    std::thread receive_reply_thread(receive_reply, socket_fd);

    

    // pthread_t writer_thread;
    // pthread_t reader_thread;
    // std::thread writer_thread(writer_main);
    // std::thread reader_thread(reader_main, src_addr, data_port, bsize);


    // pthread_create(&writer_thread, NULL, writer_main, NULL);
    // pthread_create(&reader_thread, NULL, reader_main, NULL);

    // std::thread writer_thread(writer_main);
    // std::thread reader_thread(reader_main);


    // pthread_join(reader_thread, NULL);
    // pthread_join(writer_thread, NULL);

    ld = static_cast<LockedData*>(malloc(sizeof(LockedData)));
    locked_data_init(ld);

    while (true) {
        std::cerr << "mainlock\n";
        CHECK_ERRNO(pthread_mutex_lock(&stations_mutex));
        while (stations.empty()) {
            std::cerr << "mainwait\n";
            CHECK_ERRNO(pthread_cond_wait(&no_stations, &stations_mutex));
        }
        std::cerr << "mainwriter\n";
        // std::cerr << "NASTĘPNA STACJA " << selected_station->first.name << " " << selected_station->first.mcast_addr << " " << selected_station->first.data_port << "\n";
        // //iterate over stations
        // for (auto it = stations.begin(); it != stations.end(); it++) {
        //     std::cerr << it->first.name << " " << it->first.mcast_addr << " " << it->first.data_port << "\n";
        // }
        ld->selected = true;
        ld->started_printing = false;
        // std::thread reader_thread(reader_main, "239.10.11.12", data_port, bsize);
        // std::cerr << strcmp(selected_station->first.mcast_addr, "239.10.11.12") << " " << selected_station->first.data_port << "\n";
        std::thread reader_thread(reader_main, selected_station->first.mcast_addr.c_str(), selected_station->first.data_port, bsize);
        std::thread writer_thread(writer_main);
        // std::cerr << "WYPUŚCIŁEM " << selected_station->first.name << " " << selected_station->first.mcast_addr << " " << selected_station->first.data_port << "\n";
        //iterate over stations
        // for (auto it = stations.begin(); it != stations.end(); it++) {
        //     std::cerr << it->first.name << " " << it->first.mcast_addr << " " << it->first.data_port << "\n";
        // }
        std::cerr << "mainunlock\n";
        CHECK_ERRNO(pthread_mutex_unlock(&stations_mutex));
        writer_thread.join();
        std::cerr << "mainreadjoin\n";
        reader_thread.join();
    }

    ui_thread.join();
    send_lookup_thread.join();
    receive_reply_thread.join();
    // pthread_join(send_lookup_thread, NULL);
    // pthread_join(receive_reply_thread, NULL);
    return 0;
}