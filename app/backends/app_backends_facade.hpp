#ifndef C56C8359_242F_4112_AFD8_5ED905EF2FA8
#define C56C8359_242F_4112_AFD8_5ED905EF2FA8
#include <functional>
#include <span>
#include "errno.h"
#include "proxy/v4/proxy.h"
#include <proxy/proxy.h>
#include "app_metadata_models.hpp"

namespace app::backends {

using on_metadata_fn_t = std::move_only_function<void(const frame_metadata_t &metadata)>;
using on_frame_fn_t    = std::move_only_function<void(std::span<uint8_t> frame_buffer)>;

PRO_DEF_MEM_DISPATCH(MemInit, Init);
PRO_DEF_MEM_DISPATCH(MemShutdown, Shutdown);
PRO_DEF_MEM_DISPATCH(MemSetOnMetadata, SetOnMetadata);
PRO_DEF_MEM_DISPATCH(MemSetOnFrame, SetOnFrame);
PRO_DEF_MEM_DISPATCH(MemSeekFrame, SeekFrame);

// clang-format off
struct IBackend : pro::facade_builder 
    ::add_convention<MemInit, void()>
    ::add_convention<MemShutdown, void()>
    ::add_convention<MemSetOnMetadata, void(on_metadata_fn_t)>
    ::add_convention<MemSetOnFrame, void(on_frame_fn_t)>
    ::add_convention<MemSeekFrame, error_t(size_t)>
    ::build {};
// clang-format on

}

#endif /* C56C8359_242F_4112_AFD8_5ED905EF2FA8 */
