/*
 * FlexQL Server
 * Batch-drains all incoming frames from each client before responding.
 * Streams responses incrementally (flush every 256KB) to avoid deadlock
 * when SELECT returns millions of rows.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>
#include <string>

#include "network.h"
#include "storage.h"
#include "cache.h"
#include "executor.h"

static volatile bool     g_running = true;
static Catalog          *g_catalog = nullptr;
static void on_signal(int){ g_running = false; }

/* ── Build DONE payload string ───────────────────────────────────── */
static std::string done_payload(const ExecResult &res){
    std::ostringstream d;
    d << std::fixed; d.precision(3);
    d << "OK|" << res.elapsed_ms << "ms|"
      << res.rows.size() << " row" << (res.rows.size()==1?"":"s");
    if(res.from_cache) d << "|[cache hit]";
    if(!res.err.empty()) d << "|" << res.err;
    return d.str();
}

/* ── Append one framed message to a buffer ───────────────────────── */
static void buf_append(std::string &buf, MsgType type,
                        const char *data, size_t len){
    uint32_t l = htonl((uint32_t)len);
    buf += (char)type;
    buf.append((char*)&l, 4);
    buf.append(data, len);
}

/* ── Write all bytes, handling partial writes ────────────────────── */
static bool write_all(int fd, const char *p, size_t n){
    while(n > 0){
        ssize_t w = ::write(fd, p, n);
        if(w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}

/* ── Flush out_buf to fd, clear it ──────────────────────────────── */
static bool flush_buf(int fd, std::string &buf){
    if(buf.empty()) return true;
    bool ok = write_all(fd, buf.data(), buf.size());
    buf.clear();
    return ok;
}

static const size_t FLUSH_THRESHOLD = 256 * 1024; // flush every 256 KB


/* ── Per-client read buffer ──────────────────────────────────────── */
struct Client {
    Session     session;
    std::string rbuf;
};

/* ── Process one client: drain all frames, stream responses ──────── */
/* Returns false = client disconnected                                 */
static bool process_client(int fd, Client &cl, Executor &exec){
    /* Read all available data into rbuf */
    char tmp[131072];
    while(true){
        ssize_t r = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if(r > 0){ cl.rbuf.append(tmp, (size_t)r); continue; }
        if(r == 0) return false;
        if(errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }

    std::string out;
    out.reserve(FLUSH_THRESHOLD + 64*1024);

    bool any = false;
    while(cl.rbuf.size() >= FRAME_HEADER){
        MsgType type = (MsgType)(uint8_t)cl.rbuf[0];
        uint32_t len; memcpy(&len, cl.rbuf.data()+1, 4); len = ntohl(len);
        if(len > MAX_PAYLOAD) return false;
        if(cl.rbuf.size() < FRAME_HEADER + len) break;

        std::string payload = cl.rbuf.substr(FRAME_HEADER, len);
        cl.rbuf.erase(0, FRAME_HEADER + len);

        if(type != MsgType::QUERY) continue;
        any = true;

        ExecResult res = exec.execute(payload, cl.session);

        if(!res.ok){
            buf_append(out, MsgType::ERROR, res.err.data(), res.err.size());
        } else {
            /* Stream result rows, flushing periodically to avoid deadlock */
            for(auto &row : res.rows){
                /* serialise one row: "<ncols>\t<col0>\t...\n<val0>\t...\n" */
                std::ostringstream ss;
                ss << res.col_names.size();
                for(auto &n : res.col_names) ss << '\t' << n;
                ss << '\n';
                for(size_t i=0;i<row.size();++i){if(i)ss<<'\t';ss<<row[i];}
                ss << '\n';
                std::string msg = ss.str();
                buf_append(out, MsgType::RESULT_ROW, msg.data(), msg.size());

                /* Flush when buffer is large enough to keep pipe full
                   without deadlocking the socket buffers              */
                if(out.size() >= FLUSH_THRESHOLD){
                    if(!flush_buf(fd, out)) return false;
                }
            }
            /* DONE frame */
            std::string dp = done_payload(res);
            buf_append(out, MsgType::DONE, dp.data(), dp.size());
        }

        /* Flush after every complete response */
        if(!flush_buf(fd, out)) return false;
    }

    /* Final flush for any remaining bytes */
    if(!out.empty()) return flush_buf(fd, out);
    return true;
}

int main(int argc, char *argv[]){
    int port = 9000;
    std::string data_root = "data";
    size_t cache_cap = 4096;

    for(int i=1;i<argc;++i){
        std::string a = argv[i];
        if(a.substr(0,8)=="--cache=") cache_cap=(size_t)atoi(a.c_str()+8);
        else if(a=="--cache") cache_cap=4096;
        else if(isdigit((unsigned char)a[0])&&port==9000) port=atoi(argv[i]);
        else if(a[0]!='-') data_root=a;
    }

    signal(SIGINT,on_signal); signal(SIGTERM,on_signal); signal(SIGPIPE,SIG_IGN);

    int lfd=socket(AF_INET,SOCK_STREAM,0); if(lfd<0){perror("socket");return 1;}
    int opt=1; setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    int buf=8*1024*1024;
    setsockopt(lfd,SOL_SOCKET,SO_SNDBUF,&buf,sizeof(buf));
    setsockopt(lfd,SOL_SOCKET,SO_RCVBUF,&buf,sizeof(buf));

    sockaddr_in addr{}; addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((uint16_t)port);
    if(bind(lfd,(sockaddr*)&addr,sizeof(addr))<0){perror("bind");return 1;}
    if(listen(lfd,256)<0){perror("listen");return 1;}

    fprintf(stderr,"[FlexQL] Server listening on port %d\n",port);
    fprintf(stderr,"[FlexQL] Data: %s   Cache: %zu entries\n",data_root.c_str(),cache_cap);

    Catalog  catalog(data_root); g_catalog=&catalog;
    LRUCache cache(cache_cap);
    Executor exec(catalog,cache);

    std::vector<int>        fds;
    std::map<int,Client>    clients;

    int flush_counter = 0;
    while(g_running){
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(lfd,&rfds); int maxfd=lfd;
        for(int fd:fds){FD_SET(fd,&rfds);if(fd>maxfd)maxfd=fd;}

        timeval tv{0,50000};
        int n=select(maxfd+1,&rfds,nullptr,nullptr,&tv);
        if(n<0){if(errno==EINTR)continue;break;}
        if(n==0) continue;

        if(FD_ISSET(lfd,&rfds)){
            sockaddr_in ca{}; socklen_t cl=sizeof(ca);
            int cfd=accept(lfd,(sockaddr*)&ca,&cl);
            if(cfd>=0){
                int nd=1; setsockopt(cfd,IPPROTO_TCP,TCP_NODELAY,&nd,sizeof(nd));
                int cb=8*1024*1024;
                setsockopt(cfd,SOL_SOCKET,SO_SNDBUF,&cb,sizeof(cb));
                setsockopt(cfd,SOL_SOCKET,SO_RCVBUF,&cb,sizeof(cb));
                fcntl(cfd,F_SETFL,fcntl(cfd,F_GETFL,0)|O_NONBLOCK);
                fprintf(stderr,"[FlexQL] Client connected fd=%d\n",cfd);
                fds.push_back(cfd); clients[cfd]=Client{};
            }
        }

        std::vector<int> dead;
        for(int fd:fds){
            if(!FD_ISSET(fd,&rfds)) continue;
            if(!process_client(fd,clients[fd],exec)) dead.push_back(fd);
        }
        if(++flush_counter >= 1000){ catalog.flush_all(); flush_counter=0; }
        for(int fd:dead){
            fprintf(stderr,"[FlexQL] Client disconnected fd=%d  cache: %zu hits / %zu misses\n",
                    fd,cache.hits(),cache.misses());
            close(fd); clients.erase(fd);
            fds.erase(std::remove(fds.begin(),fds.end(),fd),fds.end());
        }
    }

    catalog.flush_all();
    fprintf(stderr,"[FlexQL] Cache: %zu hits / %zu misses. Shutdown.\n",
            cache.hits(),cache.misses());
    for(int fd:fds) close(fd); close(lfd);
    return 0;
}
