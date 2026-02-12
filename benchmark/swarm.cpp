#include <iostream>
#include <vector>
#include <deque>
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
#include <algorithm>
#include <chrono>

enum State { STATE_CONNECTING, STATE_CONNECTED, STATE_DONE };

struct Client {
    int fd;
    State state = STATE_CONNECTING;
    int sent_count = 0;
    int recv_count = 0;
    std::vector<uint8_t> wbuf;
    size_t wbuf_sent = 0;
    std::vector<uint8_t> rbuf;
    
    // FIX: Queue to track the start time of every individual inflight request
    std::deque<std::chrono::steady_clock::time_point> inflight_timestamps;
};

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) { perror("fcntl get"); exit(1); }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) { perror("fcntl set"); exit(1); }
}

// Request generation (unchanged)
void fill_request(std::vector<uint8_t> &buf) {
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
    if (argc < 3) return 1;
    int k_num_clients = atoi(argv[1]);
    int k_total_requests = atoi(argv[2]);
    int reqs_per_client = k_total_requests / k_num_clients;

    std::vector<Client> clients(k_num_clients);
    std::vector<uint8_t> request_data;
    fill_request(request_data);
    
    std::vector<double> latencies;
    latencies.reserve(k_total_requests);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::map<int, Client*> fd2client;

    // FIX: Start timer AFTER setup or track connection time separately
    auto global_start = std::chrono::steady_clock::now();

    for (int i = 0; i < k_num_clients; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); continue; }
        fd_set_nb(fd);
        connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
        clients[i].fd = fd;
        clients[i].wbuf = request_data; 
        fd2client[fd] = &clients[i];
    }

    std::vector<struct pollfd> poll_args;
    int completed = 0;

    while (completed < k_num_clients) {
        poll_args.clear();
        for (auto &c : clients) {
            if (c.state == STATE_DONE) continue;
            struct pollfd pfd = {c.fd, (short)(c.state == STATE_CONNECTING ? POLLOUT : POLLIN), 0};
            if (c.state == STATE_CONNECTED && c.sent_count < reqs_per_client) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), poll_args.size(), 5000); // 5s timeout
        if (rv < 0) break;

        for (const auto &pfd : poll_args) {
            if (pfd.revents == 0) continue;
            Client *c = fd2client[pfd.fd];

            if (c->state == STATE_CONNECTING) {
                if (pfd.revents & (POLLOUT | POLLERR)) {
                    int err = 0; socklen_t len = sizeof(err);
                    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err == 0) c->state = STATE_CONNECTED;
                    else { close(c->fd); c->state = STATE_DONE; completed++; }
                }
                continue;
            }

            if (pfd.revents & POLLIN) {
                uint8_t buf[8192];
                ssize_t n = read(c->fd, buf, sizeof(buf));
                if (n > 0) {
                    c->rbuf.insert(c->rbuf.end(), buf, buf + n);
                    while (c->rbuf.size() >= 4) {
                        uint32_t len; memcpy(&len, c->rbuf.data(), 4);
                        if (c->rbuf.size() < 4 + len) break;
                        
                        // FIX: Calculate latency using the matching start timestamp
                        if (!c->inflight_timestamps.empty()) {
                            auto start_time = c->inflight_timestamps.front();
                            c->inflight_timestamps.pop_front();
                            std::chrono::duration<double, std::milli> diff = std::chrono::steady_clock::now() - start_time;
                            latencies.push_back(diff.count());
                        }
                        
                        c->rbuf.erase(c->rbuf.begin(), c->rbuf.begin() + 4 + len);
                        c->recv_count++;
                        if (c->recv_count >= reqs_per_client) {
                            close(c->fd); c->state = STATE_DONE; completed++; break;
                        }
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                    close(c->fd); c->state = STATE_DONE; completed++;
                }
            }

            if (c->state == STATE_CONNECTED && (pfd.revents & POLLOUT)) {
                // Inside the POLLOUT block:
                // New: Only allows 32 requests in flight at a time
                int max_pipeline = 32; // Example pipeline depth
                while (c->sent_count < reqs_per_client && (c->sent_count - c->recv_count) < max_pipeline){
                    // FIX: Capture timestamp at the "Intent to Send"
                    // If we are starting a new request (wbuf_sent == 0), start the timer.
                    // We use a separate 'pending_time' to store this until we confirm a write > 0.
                    if (c->wbuf_sent == 0 && c->inflight_timestamps.size() <= (c->sent_count - c->recv_count)) {
                        c->inflight_timestamps.push_back(std::chrono::steady_clock::now());
                    }

                    ssize_t n = write(c->fd, 
                                    c->wbuf.data() + c->wbuf_sent, 
                                    c->wbuf.size() - c->wbuf_sent);
                    
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Buffer is full. We keep the timestamp in the queue because
                            // the "latency clock" is already ticking for this request!
                            break; 
                        }
                        // Fatal error logic here
                        close(c->fd); c->state = STATE_DONE; completed++; 
                        break;
                    }

                    c->wbuf_sent += n;
                    
                    // Request fully flushed to kernel
                    if (c->wbuf_sent == c->wbuf.size()) {
                        c->sent_count++;
                        c->wbuf_sent = 0;
                        // Timer was already pushed at the start of this block
                    }
                }
            }
        }
    }

    auto global_end = std::chrono::steady_clock::now();
    std::chrono::duration<double> duration = global_end - global_start;
    
    // FIX: Accurate P99 percentile
    std::sort(latencies.begin(), latencies.end());
    double p99 = 0;
    if (!latencies.empty()) {
        size_t idx = (size_t)(latencies.size() * 0.99);
        if (idx >= latencies.size()) idx = latencies.size() - 1;
        p99 = latencies[idx];
    }

    printf("%.0f,%.2f\n", (double)k_total_requests / duration.count(), p99);
    return 0;
}