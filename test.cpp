// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
// C++
#include <vector>
#include <string>
#include <map>
#include <unordered_map>

using namespace std;
const size_t k_max_msg = 32 << 20;

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char *msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

void fb_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0){
        die("fcntl error");
    }
    int ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if(ret < 0){
        die("fcntl error");
    }
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

// remove from the front
static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

struct conn {
    int fd=-1;
    bool want_write=false;
    bool want_read=true;
    bool want_close=false;

    vector<uint8_t> read_buf;
    vector<uint8_t> write_buf;
};

conn* handle_accept(int fd) {
    int newfd = accept(fd, NULL, NULL);
    if(newfd < 0){
        if(errno == EAGAIN) return NULL;
        msg_errno("accept() error while listeining");
    }
    fb_set_nb(newfd);
    conn* c = new conn();
    c->fd = newfd;
    c->want_read = true;

    return c;
}

bool read_u32(const uint8_t* &cur, const uint8_t* end, uint32_t &out){
    if(cur + 4 > end) return false;
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

bool read_str(const uint8_t* &cur, const uint8_t* end, size_t n, string &out){
    if(cur + n > end) return false;
    out.assign((const char*)cur, n);
    cur += n;
    return true;
}

int32_t parse_req(const uint8_t* data, size_t len,vector<string> &out){
    const uint8_t* end = data + len;
    uint32_t nstr;
    if(!read_u32(data, end, nstr)) return -1;
    if(nstr > k_max_msg) return -1;
    while(out.size() < nstr){
        uint32_t slen;
        if(!read_u32(data, end, slen)) return -1;
        string s;
        if(!read_str(data, end, slen, s)) return -1;
        out.push_back(move(s));
    }
    return 0;
}

struct Response{
        uint32_t status=0;
        vector<uint8_t> data;
};

static map<string, string> g_data;

int32_t make_response(const Response &resp, vector<uint8_t> &out){
    uint32_t resp_len = 4 + resp.data.size();
    buf_append(out, (const uint8_t*)&resp_len, 4);
    buf_append(out, (const uint8_t*)&resp.status, 4);
    buf_append(out, resp.data.data(), resp.data.size());
    return 0;
}

void do_request(const vector<string> &cmd, Response &resp, vector<uint8_t> &out){
    if(cmd.size() == 2 && cmd[0] == "get"){
        auto it = g_data.find(cmd[1]);
        if(it == g_data.end()){
            resp.status = 1; // not found
            make_response(resp, out);
            return;
        }
        resp.status = 0; // success
        const string &val = it->second;
        resp.data.assign(val.begin(), val.end());
    } else if(cmd.size() == 3 && cmd[0] == "set"){
        g_data[cmd[1]] = move(cmd[2]);
    } else if(cmd.size() == 2 && cmd[0] == "del"){
        g_data.erase(cmd[1]);
    } else {
        resp.status = 1; // error
    }
    make_response(resp, out);
}


bool handle_one_request(conn* c){

    if(c->read_buf.size() < 4) return false;
    uint32_t len;
    memcpy(&len, c->read_buf.data(), 4);
    if(len > (32 << 20)){
        msg("too long");
        c->want_close = true;
        return false;
    }
    if(c->read_buf.size() < 4+len) return false;
    auto response=  c->read_buf.data()+4;

    vector<string> cmd;
    if(parse_req(response, len, cmd) < 0){
        msg("bad request");
        c->want_close = true;
        return false;
    }
    Response resp;
    do_request(cmd, resp, c->write_buf);
    buf_consume(c->read_buf, 4+len);    
    return true;
}



void handle_write(conn* c){
    if (c->write_buf.empty()) {
        c->want_write = false;
        return;
    }
    ssize_t rv = write(c->fd, c->write_buf.data(), c->write_buf.size());
    if(rv < 0){
        if(errno == EAGAIN) return;
        msg_errno("write() error");
        c->want_close = true;
        return;
    }
    buf_consume(c->write_buf, rv);
    if(c->write_buf.size() == 0){
        c->want_write = false;
    }
}

void handle_read(conn* c){
    uint8_t buf[64*1024];
    ssize_t rv = read(c->fd, buf, sizeof(buf));
    if(rv < 0){
        if(errno == EAGAIN) return;
        msg_errno("read() error");
        c->want_close = true;
        return;
    }
    if(rv == 0){
        msg_errno("client closed");
        c->want_close = true;
        return;
    }
    c->read_buf.insert(c->read_buf.end(), buf, buf+rv);
    while(handle_one_request(c)){}

    if(c->write_buf.size() > 0){
        c->want_write = true;
        handle_write(c);
    }
}


int main() {
    // Create a socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        die("socket()");
    }

    //set socket options
    int opt = 1;
    int ret_opt = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set up the server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(1234);
    server_addr.sin_addr.s_addr = ntohl(0);
    

    // Connect to the server
    int ret = bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if(ret < 0){
        die("bind()");
    }

    //make it non blocking using a seperate helper function
    fb_set_nb(sockfd);

    int rv=listen(sockfd, SOMAXCONN);
    if(rv < 0){
        die("listen()");
    }

    vector<struct pollfd> poll_args;
    vector<struct conn*> fd2conn;
    
    while(true){
        poll_args.clear();

        //for our listening socket
        pollfd pf_l={sockfd, POLLIN, 0};
        poll_args.push_back(pf_l);
        //for all other connections
        for(conn* c: fd2conn){
            if(!c) continue;
            pollfd pfd={c->fd, 0, 0};
            if(c->want_read) pfd.events |= POLLIN;
            if(c->want_write) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        int ret_poll = poll(poll_args.data(), poll_args.size(), -1);
        if(ret_poll < 0){
            if(errno == EINTR) continue;
            die("poll()");
        }
        if(poll_args[0].revents & POLLIN){
            conn* c=handle_accept(sockfd);
            if(!c) continue;
            int newfd = c->fd;
            if(fd2conn.size() <= (size_t)newfd){
                fd2conn.resize(newfd+1, NULL);
            }
            fd2conn[newfd] = c;
        }
        for(size_t i=1; i<poll_args.size(); i++){
            uint32_t ready = poll_args[i].revents;
            if (ready == 0) {
                continue;
            }
            conn* Conn = fd2conn[poll_args[i].fd];
            if(ready & POLLIN){
                assert(Conn->want_read);
                handle_read(Conn);
            }
            if(ready & POLLOUT){
                assert(Conn->want_write);
                handle_write(Conn);
            }
            if((ready & POLLERR) || Conn->want_close){
                close(poll_args[i].fd);
                fd2conn[poll_args[i].fd] = NULL;
                delete Conn;
            }
        }
    }
}