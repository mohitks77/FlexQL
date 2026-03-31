#pragma once
#include <cstdint>
#include <cstddef>

#define FLEXQL_OK    0
#define FLEXQL_ERROR 1

enum class MsgType : uint8_t {
    QUERY      = 1,
    RESULT_ROW = 2,
    OK         = 3,
    ERROR      = 4,
    DONE       = 5,
};

static constexpr size_t FRAME_HEADER = 5;
static constexpr size_t MAX_PAYLOAD  = 128 * 1024 * 1024;
