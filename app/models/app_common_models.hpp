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

constexpr uint8_t VERSION_MAJOR = 1;
constexpr uint8_t VERSION_MINOR = 0;
}

#endif /* B5261CB1_4CC9_4E59_9BDC_1197D7E4F905 */
