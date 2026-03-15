#ifndef B5261CB1_4CC9_4E59_9BDC_1197D7E4F905
#define B5261CB1_4CC9_4E59_9BDC_1197D7E4F905
#include <cstdint>
#include <cstddef>

namespace app {
constexpr size_t LABEL_LEN_MAX = 24;
/**
 * @brief offset of the shared memory payload
 * @note the first 256 bytes are reserved for the frame info and other useful metadata
 */
constexpr size_t SHM_PAYLOAD_OFFSET = 256;

constexpr uint8_t FRAME_TOPIC_MAGIC   = 0x7d;
constexpr uint8_t MODULE_STATUS_MAGIC = 0x5a;

constexpr uint8_t CONTROL_MESSAGE_REQUEST_MAGIC  = 0x3c;
constexpr uint8_t CONTROL_MESSAGE_RESPONSE_MAGIC = 0x3d;

constexpr int32_t CONTROL_MSG_CMD_GENERIC           = 0;
constexpr int32_t CONTROL_MSG_CMD_RESET_FRAME_COUNT = 0x1001;
constexpr int32_t CONTROL_MSG_CMD_GET_SOURCE_INFO   = 0x1002;
constexpr int32_t CONTROL_MSG_CMD_SEEK_TIMESTAMP_NS = 0x1003;
constexpr int32_t CONTROL_MSG_CMD_START_RECORDING   = 0x1004;
constexpr int32_t CONTROL_MSG_CMD_STOP_RECORDING    = 0x1005;
constexpr int32_t CONTROL_MSG_CMD_GET_RECORDING_STATUS = 0x1006;

constexpr int32_t CONTROL_RESPONSE_OK               = 0;
constexpr int32_t CONTROL_RESPONSE_UNKNOWN_CMD      = -1;
constexpr int32_t CONTROL_RESPONSE_ERROR            = -2;
constexpr int32_t CONTROL_RESPONSE_INVALID_MAGIC    = -3;
constexpr int32_t CONTROL_RESPONSE_INVALID_LABEL    = -4;
constexpr int32_t CONTROL_RESPONSE_INVALID_VERSION  = -5;
constexpr int32_t CONTROL_RESPONSE_INVALID_MSG_SIZE = -6;
constexpr int32_t CONTROL_RESPONSE_UNSUPPORTED      = -7;
constexpr int32_t CONTROL_RESPONSE_INVALID_PAYLOAD  = -8;
constexpr int32_t CONTROL_RESPONSE_OUT_OF_RANGE     = -9;

constexpr int32_t MODULE_STATUS_ONLINE       = 0xa1;
constexpr int32_t MODULE_STATUS_OFFLINE      = 0xa0;
constexpr int32_t MODULE_STATUS_STREAM_RESET = 0xb0;

constexpr uint8_t VERSION_MAJOR = 1;
constexpr uint8_t VERSION_MINOR = 0;
}

#endif /* B5261CB1_4CC9_4E59_9BDC_1197D7E4F905 */
