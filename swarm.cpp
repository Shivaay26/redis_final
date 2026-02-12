#include <iostream>
#include <vector>
#include <map>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>

// Dynamic Configuration
int k_num_clients = 1;
int k_total_requests = 1;

enum State {
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_DONE
};

struct Client {
    int fd;
    State state = STATE_CONNECTING;
    int sent_count = 0;
    int recv_count = 0;
    std::vector<uint8_t> wbuf;
    size_t wbuf_sent = 0;
    std::vector<uint8_t> rbuf;
};

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void fill_request(std::vector<uint8_t> &buf) {
    // 31 bytes: [Len][nstr][len]set[len]key[len]value
    buf.resize(31);
    uint32_t len = 27, n = 3, l1 = 3, l2 = 3, l3 = 5;
    uint8_t *ptr = buf.data();
    memcpy(ptr, &len, 4); ptr += 4;
    memcpy(ptr, &n, 4);   ptr += 4;
    memcpy(ptr, &l1, 4);  memcpy(ptr + 4, "set", 3); ptr += 7;
    memcpy(ptr, &l2, 4);  memcpy(ptr + 4, "key", 3); ptr += 7;
    memcpy(ptr, &l3, 4);  memcpy(ptr + 4, "value", 5); ptr += 9; 
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <num_clients> <total_requests>\n", argv[0]);
        return 1;
    }

    k_num_clients = atoi(argv[1]);
    k_total_requests = atoi(argv[2]);
    int reqs_per_client = k_total_requests / k_num_clients;
    if (reqs_per_client < 1) reqs_per_client = 1;
    k_total_requests = k_num_clients * reqs_per_client;

    std::vector<Client> clients(k_num_clients);
    std::vector<uint8_t> request_data;
    fill_request(request_data);
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::map<int, Client*> fd2client;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < k_num_clients; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }
        fd_set_nb(fd);
        connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
        
        clients[i].fd = fd;
        clients[i].wbuf = request_data; 
        clients[i].wbuf_sent = 0;
        fd2client[fd] = &clients[i];
    }

    std::vector<struct pollfd> poll_args;
    int completed = 0;

    while (completed < k_num_clients) {
        poll_args.clear();
        for (const auto &c : clients) {
            if (c.state == STATE_DONE) continue;
            struct pollfd pfd = {};
            pfd.fd = c.fd;
            
            if (c.state == STATE_CONNECTING) {
                pfd.events = POLLOUT;
            } else {
                pfd.events = POLLIN;
                // Only ask to write if we aren't finished sending
                if (c.sent_count < reqs_per_client) pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), poll_args.size(), 1000);
        if (rv < 0) break;

        for (const auto &pfd : poll_args) {
            if (pfd.revents == 0) continue;
            Client *c = fd2client[pfd.fd];

            // 1. Connection Completion
            if (c->state == STATE_CONNECTING) {
                if (pfd.revents & (POLLOUT | POLLERR | POLLHUP)) {
                    int err = 0; socklen_t len = sizeof(err);
                    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err) {
                        close(c->fd);
                        c->state = STATE_DONE; completed++;
                    } else {
                        c->state = STATE_CONNECTED;
                    }
                }
                continue;
            }

            // 2. Read Response
            if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
                uint8_t buf[4096];
                ssize_t n = read(c->fd, buf, sizeof(buf));
                if (n <= 0) {
                    if (errno != EAGAIN) {
                        close(c->fd); c->state = STATE_DONE; completed++;
                    }
                } else {
                    c->rbuf.insert(c->rbuf.end(), buf, buf + n);
                    while (c->rbuf.size() >= 4) {
                        uint32_t len = 0;
                        memcpy(&len, c->rbuf.data(), 4);
                        if (c->rbuf.size() < 4 + len) break;
                        c->rbuf.erase(c->rbuf.begin(), c->rbuf.begin() + 4 + len);
                        c->recv_count++;
                        if (c->recv_count >= reqs_per_client) {
                            close(c->fd); c->state = STATE_DONE; completed++; break;
                        }
                    }
                }
            }

            // 3. Write Request (Pipelining)
            if (c->state == STATE_CONNECTED && (pfd.revents & POLLOUT)) {
                // Keep writing until kernel buffer is full or we are done
                while (c->sent_count < reqs_per_client) {
                    ssize_t n = write(c->fd, 
                                      c->wbuf.data() + c->wbuf_sent, 
                                      c->wbuf.size() - c->wbuf_sent);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Buffer full
                        // Fatal error
                        close(c->fd); c->state = STATE_DONE; completed++; break;
                    }
                    c->wbuf_sent += n;
                    if (c->wbuf_sent == c->wbuf.size()) {
                        c->sent_count++;
                        c->wbuf_sent = 0;
                    }
                }
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double duration = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("%.0f\n", (double)k_total_requests / duration); 
    return 0;
}