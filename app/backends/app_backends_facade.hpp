#ifndef C56C8359_242F_4112_AFD8_5ED905EF2FA8
#define C56C8359_242F_4112_AFD8_5ED905EF2FA8
#include <functional>
#include <span>
#include <string_view>
#include "proxy/v4/proxy.h"
#include <proxy/proxy.h>
#include "app_metadata_models.hpp"

namespace app::backends {
/// @brief POSIX style error code
using error_t = int;

constexpr error_t ERR_OK  = 0;
constexpr error_t ERR_EOS = ERR_OK;

/// @brief Callback invoked once when metadata is available (first frame captured)
using on_metadata_fn_t = std::move_only_function<void(const frame_metadata_t &metadata)>;
/// @brief Callback invoked for each captured frame with frame buffer and current metadata
using on_frame_fn_t = std::move_only_function<void(std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata)>;
/// @brief Callback invoked on backend errors (e.g., capture failure, device disconnection)
using on_error_fn_t = std::move_only_function<void(error_t error_code, std::string_view message)>;

PRO_DEF_MEM_DISPATCH(MemInit, Init);
PRO_DEF_MEM_DISPATCH(MemShutdown, Shutdown);
PRO_DEF_MEM_DISPATCH(MemSetOnMetadata, SetOnMetadata);
PRO_DEF_MEM_DISPATCH(MemSetOnFrame, SetOnFrame);
PRO_DEF_MEM_DISPATCH(MemSetOnError, SetOnError);
PRO_DEF_MEM_DISPATCH(MemSeekFrame, SeekFrame);
PRO_DEF_MEM_DISPATCH(MemResetFrameCount, ResetFrameCount);

// clang-format off
struct IBackend : pro::facade_builder 
    ::add_convention<MemInit, void()>
    ::add_convention<MemShutdown, void()>
    ::add_convention<MemSetOnMetadata, void(on_metadata_fn_t)>
    ::add_convention<MemSetOnFrame, void(on_frame_fn_t)>
    ::add_convention<MemSetOnError, void(on_error_fn_t)>
    ::add_convention<MemSeekFrame, error_t(size_t)>
    ::add_convention<MemResetFrameCount, error_t()>
    ::build {};
// clang-format on

}

#endif /* C56C8359_242F_4112_AFD8_5ED905EF2FA8 */
