// clientmain.cpp
// UDP binary client Lab1b -
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h>
#include "protocol.h"

#define TIMEOUT_SEC 2
#define MAX_RETRIES 3
#define BUFSIZE 2048

using std::cout;
using std::endl;
using std::string;

// Map arithmetic codes
const char* getOperationName(int code) {
    static const char* names[] = {"unknown","add","sub","mul","div","fadd","fsub","fmul","fdiv"};
    return (code >=1 && code <=8) ? names[code] : "unknown";
}

// Exit on wrong size/protocol
void protocolErrorExit() {
    cout << "ERROR WRONG SIZE OR INCORRECT PROTOCOL" << endl;
    std::flush(std::cout);
    exit(1);
}

// Parse host:port
bool parseHostPort(const char* input, char* host, size_t hlen, char* port, size_t plen) {
    if(!input || !host || !port) return false;
    if(input[0] == '[') {
        const char* end = strstr(input, "]:");
        if(!end) return false;
        size_t len = end - (input+1);
        if(len >= hlen) return false;
        memcpy(host, input+1, len); host[len]='\0';
        strncpy(port, end+2, plen-1); port[plen-1]='\0';
    } else {
        const char* sep = strrchr(input, ':');
        if(!sep) return false;
        size_t len = sep - input;
        if(len >= hlen) return false;
        memcpy(host, input, len); host[len]='\0';
        strncpy(port, sep+1, plen-1); port[plen-1]='\0';
    }
    return true;
}

// Send message with retry and receive response
ssize_t sendWithRetry(int sock, const void* sendbuf, size_t sendlen, void* recvbuf, size_t recvlen,
                      const struct sockaddr* addr, socklen_t addrlen) {
    for(int i=0;i<MAX_RETRIES;i++){
        sendto(sock, sendbuf, sendlen, 0, addr, addrlen);
        ssize_t r = recvfrom(sock, recvbuf, recvlen, 0, NULL, NULL);
        if(r>=0) return r;
        if(errno!=EAGAIN && errno!=EWOULDBLOCK) { perror("recvfrom"); return -1; }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    if(argc!=2){ std::cerr << "Usage: " << argv[0] << " <host:port>    (supports [ipv6]:port)" << endl; return 1; }

    char host[256], port[32];
    if(!parseHostPort(argv[1], host, sizeof(host), port, sizeof(port))) {
        std::cerr << "Invalid address format. Use host:port or [ipv6]:port" << endl; return 1;
    }

    cout << "Host " << host << ", and port " << port << "." << endl;
    std::flush(cout);

    // Resolve
    struct addrinfo hints{}, *res=NULL;
    hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM;
    if(getaddrinfo(host, port, &hints, &res)!=0) { std::cerr << "ERROR: RESOLVE ISSUE" << endl; return 1; }

    int sock=-1; struct sockaddr_storage serv{}; socklen_t servlen=0;
    for(auto rp=res; rp; rp=rp->ai_next){
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if(sock<0) continue;
        memcpy(&serv,rp->ai_addr,rp->ai_addrlen); servlen=rp->ai_addrlen; break;
    }
    freeaddrinfo(res);
    if(sock<0){ cout << "ERROR: CANT CONNECT TO " << host << endl; return 1; }

    struct timeval tv{TIMEOUT_SEC,0};
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));

    // Initial handshake
    calcMessage hello{};
    hello.type = htons(22); hello.message = htonl(0); hello.protocol = htons(17);
    hello.major_version=htons(1); hello.minor_version=htons(0);

    unsigned char buffer[BUFSIZE];
    ssize_t rlen = sendWithRetry(sock,&hello,sizeof(hello),buffer,sizeof(buffer),(struct sockaddr*)&serv,servlen);
    if(rlen<0){ cout << "No response from server" << endl; close(sock); return 1; }

    if((size_t)rlen==sizeof(calcMessage)){
        calcMessage cm; memcpy(&cm,buffer,sizeof(cm));
        if(ntohl(cm.message)==2){ cout << "Server replied NOT OK" << endl; std::flush(cout); close(sock); return 0; }
        protocolErrorExit();
    } else if((size_t)rlen==sizeof(calcProtocol)){
        calcProtocol task; memcpy(&task,buffer,sizeof(task));

        if(ntohs(task.type)!=1 || ntohs(task.major_version)!=1 || ntohs(task.minor_version)!=0) protocolErrorExit();

        bool isFloat = (ntohl(task.arith)>=5);
        int32_t intRes=0; double flRes=0.0;
        int32_t in1=ntohl(task.inValue1), in2=ntohl(task.inValue2);
        double f1=task.flValue1, f2=task.flValue2;
        uint32_t arith = ntohl(task.arith);

        if(!isFloat){
            switch(arith){ case 1:intRes=in1+in2; break; case 2:intRes=in1-in2; break;
                            case 3:intRes=in1*in2; break; case 4:intRes=(in2? in1/in2:0); break; }
            cout << "ASSIGNMENT: " << getOperationName(arith) << " " << in1 << " " << in2 << endl;
        } else {
            switch(arith){ case 5: flRes=f1+f2; break; case 6: flRes=f1-f2; break;
                            case 7: flRes=f1*f2; break; case 8: flRes=(f2!=0.0? f1/f2:0.0); break; }
            cout << "ASSIGNMENT: " << getOperationName(arith) << " " << std::setprecision(8) << f1 << " " << f2 << endl;
        }
        std::flush(cout);

        // prepare reply
        calcProtocol reply{}; reply.type=htons(2); reply.major_version=htons(1); reply.minor_version=htons(0);
        reply.id = task.id; reply.arith = task.arith; reply.inValue1=task.inValue1; reply.inValue2=task.inValue2;
        if(!isFloat) reply.inResult=htonl(intRes);
        else{ reply.flValue1=f1; reply.flValue2=f2; reply.flResult=flRes; }

        rlen = sendWithRetry(sock,&reply,sizeof(reply),buffer,sizeof(buffer),(struct sockaddr*)&serv,servlen);
        if(rlen!=sizeof(calcMessage)) protocolErrorExit();

        calcMessage finalMsg; memcpy(&finalMsg,buffer,sizeof(finalMsg));
        if(!isFloat) cout << (ntohl(finalMsg.message)==1?"OK":"NOT OK") << " (myresult=" << intRes << ")" << endl;
        else cout << (ntohl(finalMsg.message)==1?"OK":"NOT OK") << " (myresult=" << std::setprecision(8) << flRes << ")" << endl;
        std::flush(cout);
    } else protocolErrorExit();

    close(sock);
    return 0;
}