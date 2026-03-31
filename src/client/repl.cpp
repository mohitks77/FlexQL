/*
 * FlexQL interactive REPL
 * - Prompt shows current database: flexql [MYDB]>
 * - Shows query time + cache-hit indicator after every result
 * - Multi-line query support (waits for semicolon)
 * - .help  .exit  .quit  meta-commands
 */
#include "flexql.h"
#include "network.h"
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static std::string g_last_info;
static std::string g_current_db;

static std::vector<std::string> split_ch(const std::string&s,char d){
    std::vector<std::string> o; std::string c;
    for(char ch:s){if(ch==d){o.push_back(c);c.clear();}else c+=ch;}
    o.push_back(c); return o;}

static int print_row(void*,int ncols,char**vals,char**names){
    for(int i=0;i<ncols;++i)
        printf("  %-20s = %s\n",names[i]?names[i]:"(null)",vals[i]?vals[i]:"NULL");
    printf("\n"); return 0;}

static int repl_exec(int fd,const char*sql,int(*cb)(void*,int,char**,char**)){
    if(!send_frame(fd,MsgType::QUERY,sql)) return FLEXQL_ERROR;
    g_last_info.clear();
    while(true){
        MsgType type; std::string payload;
        if(!recv_frame(fd,type,payload)){fprintf(stderr,"Connection lost\n");return FLEXQL_ERROR;}
        if(type==MsgType::ERROR){fprintf(stderr,"\033[31mError:\033[0m %s\n",payload.c_str());return FLEXQL_ERROR;}
        if(type==MsgType::DONE){
            auto p=split_ch(payload,'|');
            /* p[0]=OK  p[1]=time  p[2]=rows  p[3]=cache_hit? */
            std::string info;
            if(p.size()>=3) info=p[1]+" | "+p[2];
            if(p.size()>=4&&p[3].find("cache")!=std::string::npos)
                info+="\033[36m [cached]\033[0m";
            g_last_info=info;
            return FLEXQL_OK;}
        if(type==MsgType::RESULT_ROW&&cb){
            auto lines=split_ch(payload,'\n'); if(lines.empty())continue;
            auto hp=split_ch(lines[0],'\t');   if(hp.empty())continue;
            int nc=std::stoi(hp[0]);
            std::vector<std::string> cn,vn;
            for(int i=1;i<=nc&&i<(int)hp.size();++i) cn.push_back(hp[i]);
            while((int)cn.size()<nc) cn.push_back("");
            if(lines.size()>=2) vn=split_ch(lines[1],'\t');
            while((int)vn.size()<nc) vn.push_back("");
            std::vector<char*> cp,vp;
            for(auto&n:cn) cp.push_back(const_cast<char*>(n.c_str()));
            for(auto&v:vn) vp.push_back(const_cast<char*>(v.c_str()));
            cb(nullptr,nc,vp.data(),cp.data());}}
}

static void track_use(const std::string&sql){
    std::string u=sql; for(char&c:u) c=(char)toupper((unsigned char)c);
    auto p=u.find("USE ");
    if(p==std::string::npos)return;
    std::string rest=sql.substr(p+4);
    while(!rest.empty()&&isspace((unsigned char)rest.front()))rest.erase(rest.begin());
    while(!rest.empty()&&(rest.back()==';'||isspace((unsigned char)rest.back())))rest.pop_back();
    if(!rest.empty()){for(char&c:rest)c=(char)toupper((unsigned char)c);g_current_db=rest;}}

static void print_prompt(bool cont=false){
    if(cont){printf("       -> ");fflush(stdout);return;}
    if(g_current_db.empty()) printf("\033[1mflexql\033[0m> ");
    else printf("\033[1mflexql\033[0m [\033[33m%s\033[0m]> ",g_current_db.c_str());
    fflush(stdout);}

static void print_help(){
    printf("\n\033[1mFlexQL Commands\033[0m\n"
        "  CREATE DATABASE <n>;                     Create a database\n"
        "  USE <n>;                                 Select a database\n"
        "  SHOW DATABASES;                          List all databases\n"
        "  SHOW TABLES;                             List tables in current DB\n"
        "  CREATE TABLE t(col TYPE [PK][NOT NULL]); Create a table\n"
        "  DESCRIBE <table>;                        Show schema\n"
        "  INSERT INTO t VALUES(...);               Insert a row\n"
        "  INSERT INTO t(c1,c2) VALUES(...);        Named-column insert\n"
        "  SELECT * FROM t;                         All rows\n"
        "  SELECT c1,c2 FROM t WHERE col op val;    Filtered query\n"
        "  SELECT * FROM a INNER JOIN b ON a.c=b.c; Join two tables\n"
        "  SELECT * FROM t ORDER BY col [ASC|DESC]; Sorted results\n"
        "  SELECT * FROM t LIMIT n;                 Limit results\n"
        "  UPDATE t SET col=val [WHERE ...];        Update rows\n"
        "  DELETE FROM t [WHERE ...];               Delete rows\n"
        "  DROP TABLE <n>;                          Drop table\n"
        "  DROP DATABASE <n>;                       Drop database\n"
        "\n\033[1mData Types:\033[0m  INT   DECIMAL   VARCHAR   DATETIME\n"
        "\033[1mDATETIME:\033[0m  '2024-06-15 14:30:00'  NOW()  CURRENT_TIMESTAMP\n"
        "\033[1mExpiry:\033[0m  Rows expire 1h after insert by default.\n"
        "        Override: INSERT INTO t VALUES(...) EXPIRES <unix_ts>;\n"
        "\n\033[1mMeta:\033[0m  .help  .exit  .quit\n\n");}

static int raw_connect(const char*host,int port){
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0)return -1;
    struct hostent*he=gethostbyname(host);
    if(!he){close(fd);return -1;}
    sockaddr_in sa{}; sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)port);
    memcpy(&sa.sin_addr,he->h_addr_list[0],(size_t)he->h_length);
    if(connect(fd,(sockaddr*)&sa,sizeof(sa))<0){close(fd);return -1;}
    return fd;}

int main(int argc,char*argv[]){
    const char*host="127.0.0.1"; int port=9000;
    if(argc>=3){host=argv[1];port=atoi(argv[2]);}
    else if(argc==2){port=atoi(argv[1]);}

    int fd=raw_connect(host,port);
    if(fd<0){
        fprintf(stderr,"Cannot connect to FlexQL at %s:%d\n",host,port);
        fprintf(stderr,"Start server: ./bin/flexql-server %d\n",port);
        return 1;}

    printf("\033[1mFlexQL\033[0m connected to %s:%d\n",host,port);
    printf("Type \033[1m.help\033[0m for commands, \033[1m.exit\033[0m to quit.\n\n");

    std::string line,query;
    print_prompt();

    while(std::getline(std::cin,line)){
        if(line==".exit"||line==".quit"){printf("Bye!\n");break;}
        if(line==".help"){print_help();print_prompt();continue;}

        query+=line+" ";
        if(query.find(';')==std::string::npos){print_prompt(true);continue;}

        std::string sql=query; query.clear();
        while(!sql.empty()&&(sql.back()==';'||sql.back()==' '))sql.pop_back();
        track_use(sql);

        int rc=repl_exec(fd,sql.c_str(),print_row);
        if(rc==FLEXQL_OK&&!g_last_info.empty())
            printf("\033[2mQuery OK  (%s)\033[0m\n\n",g_last_info.c_str());
        else if(rc!=FLEXQL_OK)
            printf("\n");
        print_prompt();}

    close(fd); return 0;}
