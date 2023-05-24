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

#include "err.h"
#include "utils.h"

struct LockedData *ld;

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

int main(int argc, char *argv[]) {
    uint16_t data_port = 29978;
    size_t bsize = 655368;
    const char* src_addr = NULL;
    int flag;
    while((flag = getopt(argc, argv, "a:P:b:")) != -1) {
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
    char *receive_buf = static_cast<char*>(std::malloc(65536 + 16));

    int socket_fd = bind_socket(data_port);

    size_t psize;
    struct sockaddr_in client_address;
    psize = read_message(socket_fd, &client_address, receive_buf, 65535, src_addr) - 16;
    uint64_t session_id;
    uint64_t byte_zero;
    memcpy(&session_id, receive_buf, sizeof(uint64_t));
    memcpy(&byte_zero, receive_buf + sizeof(uint64_t), sizeof(uint64_t));

    session_id = be64toh(session_id);
    byte_zero = be64toh(byte_zero);

    ld = static_cast<struct LockedData*>(std::malloc(sizeof(struct LockedData)));
    locked_data_init(ld, bsize, psize, socket_fd, session_id, byte_zero, src_addr);
    memcpy(ld->data, receive_buf + 2 * sizeof(uint64_t), psize);
    ld->received[0] = true;

    pthread_t writer_thread;
    pthread_t reader_thread;
    pthread_create(&writer_thread, NULL, writer_main, NULL);
    pthread_create(&reader_thread, NULL, reader_main, NULL);


    pthread_join(reader_thread, NULL);
    pthread_join(writer_thread, NULL);
    return 0;
}