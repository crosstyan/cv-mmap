#ifndef D42189DA_FD30_4BE8_BD6F_21391FFEF2C2
#define D42189DA_FD30_4BE8_BD6F_21391FFEF2C2

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "app_backends_dummy.hpp"
#ifdef WITH_BACKEND_OPENCV
#include "app_backends_opencv.hpp"
#endif
#ifdef WITH_BACKEND_GSTREAMER
#include "app_backends_gst.hpp"
#endif
#ifdef WITH_BACKEND_MCAP
#include "app_backends_mcap.hpp"
#endif
#ifdef WITH_BACKEND_ZED
#include "app_backends_zed.hpp"
#endif

namespace app::backends {
namespace detail {

template <typename T>
using backend_ptr = std::unique_ptr<T>;

using backend_storage_t = std::variant<
	std::monostate,
	backend_ptr<DummyBackend>
#ifdef WITH_BACKEND_OPENCV
	,
	backend_ptr<OpenCVBackend>
#endif
#ifdef WITH_BACKEND_GSTREAMER
	,
	backend_ptr<GStreamerBackend>
#endif
#ifdef WITH_BACKEND_MCAP
	,
	backend_ptr<McapBackend>
#endif
#ifdef WITH_BACKEND_ZED
	,
	backend_ptr<ZedBackend>
#endif
	>;

} // namespace detail

class BackendHandle {
public:
	BackendHandle() = default;
	BackendHandle(const BackendHandle &) = delete;
	BackendHandle &operator=(const BackendHandle &) = delete;
	BackendHandle(BackendHandle &&) noexcept = default;
	BackendHandle &operator=(BackendHandle &&) noexcept = default;
	~BackendHandle() = default;

	template <typename Backend, typename... Args>
	void emplace(Args &&...args) {
		storage_ = std::make_unique<Backend>(std::forward<Args>(args)...);
	}

	bool has_value() const noexcept {
		return !std::holds_alternative<std::monostate>(storage_);
	}

	void Init() {
		visit_active_void([](auto &backend) { backend.Init(); });
	}

	void Shutdown() {
		visit_active_void([](auto &backend) { backend.Shutdown(); });
	}

	void SetOnMetadata(on_metadata_fn_t on_metadata) {
		visit_active_void([&](auto &backend) { backend.SetOnMetadata(std::move(on_metadata)); });
	}

	void SetOnFrame(on_frame_fn_t on_frame) {
		visit_active_void([&](auto &backend) { backend.SetOnFrame(std::move(on_frame)); });
	}

	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking) {
		visit_active_void([&](auto &backend) { backend.SetOnBodyTracking(std::move(on_body_tracking)); });
	}

	void SetOnError(on_error_fn_t on_error) {
		visit_active_void([&](auto &backend) { backend.SetOnError(std::move(on_error)); });
	}

	source_info_t GetSourceInfo() {
		return visit_active_result<source_info_t>([](auto &backend) { return backend.GetSourceInfo(); });
	}

	error_t ResetFrameCount() {
		return visit_active_result<error_t>([](auto &backend) { return backend.ResetFrameCount(); });
	}

	template <typename Backend>
	Backend *get_if() noexcept {
		auto *ptr = std::get_if<detail::backend_ptr<Backend>>(&storage_);
		return ptr ? ptr->get() : nullptr;
	}

	template <typename Backend>
	const Backend *get_if() const noexcept {
		auto *ptr = std::get_if<detail::backend_ptr<Backend>>(&storage_);
		return ptr ? ptr->get() : nullptr;
	}

private:
	template <typename Fn>
	void visit_active_void(Fn &&fn) {
		ensure_active();
		std::visit(
			[&](auto &backend_ptr) {
				using storage_type = std::decay_t<decltype(backend_ptr)>;
				if constexpr (std::is_same_v<storage_type, std::monostate>) {
					throw std::logic_error("backend handle used without an active backend");
				} else {
					fn(*backend_ptr);
				}
			},
			storage_);
	}

	template <typename Result, typename Fn>
	Result visit_active_result(Fn &&fn) {
		ensure_active();
		return std::visit(
			[&](auto &backend_ptr) -> Result {
				using storage_type = std::decay_t<decltype(backend_ptr)>;
				if constexpr (std::is_same_v<storage_type, std::monostate>) {
					throw std::logic_error("backend handle used without an active backend");
				} else {
					return fn(*backend_ptr);
				}
			},
			storage_);
	}

	void ensure_active() const {
		if (!has_value()) {
			throw std::logic_error("backend handle used without an active backend");
		}
	}

	detail::backend_storage_t storage_;
};

} // namespace app::backends

#endif /* D42189DA_FD30_4BE8_BD6F_21391FFEF2C2 */
