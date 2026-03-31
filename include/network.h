#pragma once
#include "common.h"
#include <string>
bool send_frame(int fd, MsgType type, const std::string &payload);
bool recv_frame(int fd, MsgType &type, std::string &payload);
