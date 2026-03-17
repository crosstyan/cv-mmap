#ifndef C56C8359_242F_4112_AFD8_5ED905EF2FA8
#define C56C8359_242F_4112_AFD8_5ED905EF2FA8
#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/functional.hpp>
#include <span>
#include <string>
#include <string_view>
#include "proxy/v4/proxy.h"
#include <proxy/proxy.h>
#include "app_metadata_models.hpp"
#include <cvmmap/ipc.hpp>
#include <cvmmap/backend_types.hpp>

namespace app::backends {

/// @brief Callback invoked once when metadata is available (first frame captured)
using on_metadata_fn_t = cvmmap::move_only_function<void(const frame_metadata_t &metadata)>;
/// @brief Callback invoked for each captured frame with frame buffer and current metadata
using on_frame_fn_t = cvmmap::move_only_function<void(std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata)>;
using on_body_tracking_fn_t = cvmmap::move_only_function<void(const cvmmap::body_tracking_frame_t &frame)>;
/// @brief Callback invoked on backend errors (e.g., capture failure, device disconnection)
using on_error_fn_t = cvmmap::move_only_function<void(error_t error_code, std::string_view message)>;

PRO_DEF_MEM_DISPATCH(MemInit, Init);
PRO_DEF_MEM_DISPATCH(MemShutdown, Shutdown);
PRO_DEF_MEM_DISPATCH(MemSetOnMetadata, SetOnMetadata);
PRO_DEF_MEM_DISPATCH(MemSetOnFrame, SetOnFrame);
PRO_DEF_MEM_DISPATCH(MemSetOnBodyTracking, SetOnBodyTracking);
PRO_DEF_MEM_DISPATCH(MemSetOnError, SetOnError);
PRO_DEF_MEM_DISPATCH(MemGetSourceInfo, GetSourceInfo);
PRO_DEF_MEM_DISPATCH(MemSeekTimestamp, SeekTimestampNs);
PRO_DEF_MEM_DISPATCH(MemResetFrameCount, ResetFrameCount);

// clang-format off
struct ISeekableBackend : pro::facade_builder
	::add_convention<MemSeekTimestamp, cvmmap::expected<seek_result_t, error_t>(uint64_t)>
	::build {};

struct IBackend : pro::facade_builder
	::add_facade<ISeekableBackend>
	::add_convention<MemInit, void()>
	::add_convention<MemShutdown, void()>
	::add_convention<MemSetOnMetadata, void(on_metadata_fn_t)>
	::add_convention<MemSetOnFrame, void(on_frame_fn_t)>
	::add_convention<MemSetOnBodyTracking, void(on_body_tracking_fn_t)>
	::add_convention<MemSetOnError, void(on_error_fn_t)>
	::add_convention<MemGetSourceInfo, source_info_t()>
	::add_convention<MemResetFrameCount, error_t()>
	::add_skill<pro::skills::rtti>
	::build {};
// clang-format on

} // namespace app::backends

#endif /* C56C8359_242F_4112_AFD8_5ED905EF2FA8 */
