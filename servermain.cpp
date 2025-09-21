// servermain.cpp

#include <bits/stdc++.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "protocol.h"
#include "calcLib.h"

using namespace std;
using Clock = chrono::steady_clock;

class Task {
public:
    sockaddr_storage addr;
    socklen_t addrlen;
    uint32_t id;
    bool is_float;
    int32_t expected_int;
    double expected_double;
    Clock::time_point timestamp;

    Task(sockaddr_storage a, socklen_t l, uint32_t tid, bool f, int32_t ei, double ed)
        : addr(a), addrlen(l), id(tid), is_float(f), expected_int(ei), expected_double(ed) {
        timestamp = Clock::now();
    }
};

static unordered_map<uint32_t, Task> tasks;
static int srv_sock = -1;
static bool stop_server = false;

static void handle_sig(int) { stop_server = true; }

static string addr_to_string(const sockaddr_storage &ss) {
    char host[INET6_ADDRSTRLEN] = {0};
    char port[8] = {0};
    if (ss.ss_family == AF_INET) {
        const sockaddr_in *s = (const sockaddr_in*)&ss;
        inet_ntop(AF_INET, &s->sin_addr, host, sizeof(host));
        snprintf(port, sizeof(port), "%u", ntohs(s->sin_port));
    } else {
        const sockaddr_in6 *s6 = (const sockaddr_in6*)&ss;
        inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof(host));
        snprintf(port, sizeof(port), "%u", ntohs(s6->sin6_port));
    }
    return string(host) + ":" + string(port);
}

static bool same_sockaddr(const sockaddr_storage &a, const sockaddr_storage &b) {
    if (a.ss_family != b.ss_family) return false;
    if (a.ss_family == AF_INET) {
        const sockaddr_in *pa = (const sockaddr_in*)&a;
        const sockaddr_in *pb = (const sockaddr_in*)&b;
        return pa->sin_port == pb->sin_port && pa->sin_addr.s_addr == pb->sin_addr.s_addr;
    } else {
        const sockaddr_in6 *pa = (const sockaddr_in6*)&a;
        const sockaddr_in6 *pb = (const sockaddr_in6*)&b;
        return pa->sin6_port == pb->sin6_port &&
               memcmp(&pa->sin6_addr, &pb->sin6_addr, sizeof(in6_addr)) == 0;
    }
}

static uint32_t new_id() {
    uint32_t id;
    do {
        id = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    } while (id == 0 || tasks.find(id) != tasks.end());
    return id;
}

static int arith_code_from_name(const string &op) {
    if (op=="add") return 1;
    if (op=="sub") return 2;
    if (op=="mul") return 3;
    if (op=="div") return 4;
    if (op=="fadd") return 5;
    if (op=="fsub") return 6;
    if (op=="fmul") return 7;
    if (op=="fdiv") return 8;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s <IP:PORT>\n", argv[0]);
        return 1;
    }

    srand(time(nullptr));
    initCalcLib();

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    string arg = argv[1], host, port;
    if (!arg.empty() && arg.front() == '[') {
        auto pos = arg.find("]:");
        if (pos == string::npos) { printf("Invalid address\n"); return 1; }
        host = arg.substr(1, pos-1);
        port = arg.substr(pos+2);
    } else {
        auto pos = arg.rfind(':');
        if (pos == string::npos) { printf("Invalid address\n"); return 1; }
        host = arg.substr(0, pos);
        port = arg.substr(pos+1);
    }

    struct addrinfo hints{}, *res=nullptr, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        printf("getaddrinfo: %s\n", gai_strerror(rc));
        return 1;
    }

    srv_sock = -1;
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
        srv_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (srv_sock < 0) continue;
        int yes = 1;
        setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
        setsockopt(srv_sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
        if (bind(srv_sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(srv_sock);
        srv_sock = -1;
    }
    if (srv_sock < 0) {
        perror("bind/socket");
        freeaddrinfo(res);
        return 1;
    }

    freeaddrinfo(res);

    printf("Server started on %s:%s\n", host.c_str(), port.c_str());
    fflush(stdout);

    const size_t MSG_SZ = sizeof(struct calcMessage);
    const size_t PROTO_SZ = sizeof(struct calcProtocol);
    printf("calcProtocol = %zu bytes\n", PROTO_SZ);
    printf("calcMessage = %zu bytes\n", MSG_SZ);

    unsigned char buf[2048];

    while (!stop_server) {
        // Cleanup expired jobs
        auto now = Clock::now();
        vector<uint32_t> expired;
        for (auto &kv : tasks) {
            if (chrono::duration_cast<chrono::seconds>(now - kv.second.timestamp).count() >= 10)
                expired.push_back(kv.first);
        }
        for (uint32_t id : expired) {
            printf("Job %u timed out and removed.\n", id);
            tasks.erase(id);
        }

        // select
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv_sock, &rfds);
        timeval tv{0, 200000}; // 200ms
        int sret = select(srv_sock+1, &rfds, nullptr, nullptr, &tv);
        if (sret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sret == 0) continue;

        sockaddr_storage cliaddr;
        socklen_t cliaddr_len = sizeof(cliaddr);
        ssize_t n = recvfrom(srv_sock, buf, sizeof(buf), 0, (sockaddr*)&cliaddr, &cliaddr_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            continue;
        }

        string client_str = addr_to_string(cliaddr);
        printf("Received %zd bytes from %s\n", n, client_str.c_str());
        fflush(stdout);

        if ((size_t)n == MSG_SZ) {
            calcMessage cm;
            memcpy(&cm, buf, MSG_SZ);

            uint16_t cm_type = ntohs(cm.type);
            uint32_t cm_message = ntohl(cm.message);
            uint16_t cm_protocol = ntohs(cm.protocol);
            uint16_t cm_maj = ntohs(cm.major_version);
            uint16_t cm_min = ntohs(cm.minor_version);

            if (!(cm_type == 22 && cm_message == 0 && cm_protocol == 17 && cm_maj == 1 && cm_min == 0)) {
                printf("ERROR WRONG SIZE OR INCORRECT PROTOCOL from %s\n", client_str.c_str());
                fflush(stdout);
                continue;
            }

            calcProtocol cp{};
            cp.type = htons(1);
            cp.major_version = htons(1);
            cp.minor_version = htons(0);
            uint32_t id = new_id();
            cp.id = htonl(id);

            string op = randomType();
            int arith = arith_code_from_name(op);
            cp.arith = htonl(arith);

            Task task(cliaddr, cliaddr_len, id, arith >= 5, 0, 0.0);

            if (arith >=1 && arith <=4) {
                int v1 = randomInt(), v2 = randomInt(), res = 0;
                if (arith==1) res=v1+v2;
                else if (arith==2) res=v1-v2;
                else if (arith==3) res=v1*v2;
                else if (arith==4) res=(v2==0?0:v1/v2);
                task.expected_int = res;
                cp.inValue1 = htonl(v1);
                cp.inValue2 = htonl(v2);
                cp.inResult = htonl(0);
            } else {
                double f1=randomFloat(), f2=randomFloat(), fres=0;
                if (arith==5) fres=f1+f2;
                else if (arith==6) fres=f1-f2;
                else if (arith==7) fres=f1*f2;
                else if (arith==8) fres=(f2==0?0:f1/f2);
                task.expected_double = fres;
                cp.flValue1=f1;
                cp.flValue2=f2;
                cp.flResult=0.0;
            }

            tasks.emplace(id, task);

            ssize_t sent = sendto(srv_sock, &cp, sizeof(cp), 0, (sockaddr*)&cliaddr, cliaddr_len);
            if (sent != (ssize_t)sizeof(cp)) perror("sendto assignment");
            continue;
        }

        if ((size_t)n == PROTO_SZ) {
            calcProtocol cp;
            memcpy(&cp, buf, PROTO_SZ);
            uint16_t type = ntohs(cp.type);
            uint16_t maj = ntohs(cp.major_version);
            uint16_t min = ntohs(cp.minor_version);
            uint32_t id = ntohl(cp.id);

            if (!(type==2 && maj==1 && min==0)) {
                printf("ERROR WRONG SIZE OR INCORRECT PROTOCOL from %s\n", client_str.c_str());
                continue;
            }

            auto it = tasks.find(id);
            if (it==tasks.end()) {
                calcMessage resp{};
                resp.type=htons(1);
                resp.message=htonl(2);
                resp.protocol=htons(17);
                resp.major_version=htons(1);
                resp.minor_version=htons(0);
                sendto(srv_sock,&resp,sizeof(resp),0,(sockaddr*)&cliaddr,cliaddr_len);
                continue;
            }

            if (!same_sockaddr(it->second.addr, cliaddr)) {
                calcMessage resp{};
                resp.type=htons(1);
                resp.message=htonl(2);
                resp.protocol=htons(17);
                resp.major_version=htons(1);
                resp.minor_version=htons(0);
                sendto(srv_sock,&resp,sizeof(resp),0,(sockaddr*)&cliaddr,cliaddr_len);
                continue;
            }

            bool ok=false;
            if (!it->second.is_float) {
                int32_t client_res = (int32_t)ntohl((uint32_t)cp.inResult);
                if (client_res == it->second.expected_int) ok=true;
            } else {
                double client_res = cp.flResult;
                if (fabs(client_res - it->second.expected_double)<0.0001) ok=true;
            }

            tasks.erase(it);

            calcMessage finalm{};
            finalm.type=htons(1);
            finalm.message=htonl(ok?1:2);
            finalm.protocol=htons(17);
            finalm.major_version=htons(1);
            finalm.minor_version=htons(0);
            ssize_t s=sendto(srv_sock,&finalm,sizeof(finalm),0,(sockaddr*)&cliaddr,cliaddr_len);
            if (s != (ssize_t)sizeof(finalm)) perror("sendto final");
            continue;
        }

        printf("ERROR WRONG SIZE OR INCORRECT PROTOCOL from %s\n", client_str.c_str());
    }

    if (srv_sock>=0) close(srv_sock);
    return 0;
}