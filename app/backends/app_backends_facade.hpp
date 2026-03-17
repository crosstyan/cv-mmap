#ifndef C56C8359_242F_4112_AFD8_5ED905EF2FA8
#define C56C8359_242F_4112_AFD8_5ED905EF2FA8
#include <cvmmap/compat/functional.hpp>
#include <span>
#include <string>
#include <string_view>
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

} // namespace app::backends

#endif /* C56C8359_242F_4112_AFD8_5ED905EF2FA8 */
