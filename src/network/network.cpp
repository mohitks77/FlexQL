#include "network.h"
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>

static bool write_all(int fd,const char*b,size_t n){
    while(n>0){ssize_t w=::write(fd,b,n);if(w<=0)return false;b+=w;n-=w;}return true;}
static bool read_all(int fd,char*b,size_t n){
    while(n>0){ssize_t r=::read(fd,b,n);if(r<=0)return false;b+=r;n-=r;}return true;}

bool send_frame(int fd,MsgType type,const std::string&payload){
    char hdr[FRAME_HEADER]; hdr[0]=(char)type;
    uint32_t len=htonl((uint32_t)payload.size()); memcpy(hdr+1,&len,4);
    if(!write_all(fd,hdr,FRAME_HEADER))return false;
    if(!payload.empty())return write_all(fd,payload.data(),payload.size());
    return true;
}
bool recv_frame(int fd,MsgType&type,std::string&payload){
    char hdr[FRAME_HEADER]; if(!read_all(fd,hdr,FRAME_HEADER))return false;
    type=(MsgType)(uint8_t)hdr[0];
    uint32_t len;memcpy(&len,hdr+1,4);len=ntohl(len);
    if(len>MAX_PAYLOAD)return false;
    payload.resize(len);
    if(len>0)return read_all(fd,&payload[0],len);
    return true;
}
