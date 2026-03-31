#include "flexql.h"
#include "network.h"
#include "common.h"

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string>
#include <vector>
#include <sstream>

/* ── opaque handle ────────────────────────────────────────────────── */
struct FlexQL {
    int sockfd = -1;
};

/* ── helpers ──────────────────────────────────────────────────────── */
static std::vector<std::string> split_ch(const std::string &s, char d){
    std::vector<std::string> out;
    std::string cur;
    for(char c:s){
        if(c==d){out.push_back(cur);cur.clear();}
        else cur+=c;
    }
    out.push_back(cur);
    return out;
}

/* ── flexql_open ─────────────────────────────────────────────────── */
int flexql_open(const char *host, int port, FlexQL **db){
    if(!host||!db) return FLEXQL_ERROR;
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) return FLEXQL_ERROR;

    struct hostent *he=gethostbyname(host);
    if(!he){close(fd);return FLEXQL_ERROR;}

    sockaddr_in sa{};
    sa.sin_family=AF_INET;
    sa.sin_port  =htons((uint16_t)port);
    memcpy(&sa.sin_addr,he->h_addr_list[0],(size_t)he->h_length);

    if(connect(fd,(sockaddr*)&sa,sizeof(sa))<0){close(fd);return FLEXQL_ERROR;}

    *db=new FlexQL();
    (*db)->sockfd=fd;
    return FLEXQL_OK;
}

/* ── flexql_close ────────────────────────────────────────────────── */
int flexql_close(FlexQL *db){
    if(!db) return FLEXQL_ERROR;
    if(db->sockfd>=0) close(db->sockfd);
    delete db;
    return FLEXQL_OK;
}

/* ── flexql_exec ─────────────────────────────────────────────────── */
int flexql_exec(FlexQL *db, const char *sql,
                int(*callback)(void*,int,char**,char**),
                void *arg, char **errmsg){
    if(!db||db->sockfd<0){
        if(errmsg)*errmsg=strdup("Invalid database handle");
        return FLEXQL_ERROR;
    }
    if(!sql){
        if(errmsg)*errmsg=strdup("NULL SQL string");
        return FLEXQL_ERROR;
    }

    if(!send_frame(db->sockfd,MsgType::QUERY,sql)){
        if(errmsg)*errmsg=strdup("Failed to send query");
        return FLEXQL_ERROR;
    }

    while(true){
        MsgType type; std::string payload;
        if(!recv_frame(db->sockfd,type,payload)){
            if(errmsg)*errmsg=strdup("Connection lost");
            return FLEXQL_ERROR;
        }

        if(type==MsgType::ERROR){
            if(errmsg)*errmsg=strdup(payload.c_str());
            return FLEXQL_ERROR;
        }
        if(type==MsgType::DONE) return FLEXQL_OK;

        if(type==MsgType::RESULT_ROW&&callback){
            /* parse: header line + value line */
            auto lines=split_ch(payload,'\n');
            if(lines.empty()) continue;
            auto hparts=split_ch(lines[0],'\t');
            if(hparts.empty()) continue;
            int ncols=std::stoi(hparts[0]);

            std::vector<std::string> cnames;
            for(int i=1;i<=ncols&&i<(int)hparts.size();++i)
                cnames.push_back(hparts[i]);
            while((int)cnames.size()<ncols) cnames.push_back("");

            std::vector<std::string> vnames;
            if(lines.size()>=2) vnames=split_ch(lines[1],'\t');
            while((int)vnames.size()<ncols) vnames.push_back("");

            std::vector<char*> cp,vp;
            for(auto &n:cnames) cp.push_back(const_cast<char*>(n.c_str()));
            for(auto &v:vnames) vp.push_back(const_cast<char*>(v.c_str()));

            int cb_ret=callback(arg,ncols,vp.data(),cp.data());
            if(cb_ret!=0){
                /* drain */
                while(true){
                    MsgType t2;std::string p2;
                    if(!recv_frame(db->sockfd,t2,p2)) break;
                    if(t2==MsgType::DONE||t2==MsgType::ERROR) break;
                }
                return FLEXQL_OK;
            }
        }
    }
}

/* ── flexql_free ─────────────────────────────────────────────────── */
void flexql_free(void *ptr){ free(ptr); }
