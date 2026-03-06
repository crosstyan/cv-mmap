#pragma once

#include "ipc.hpp"
#include "target.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace cvmmap {

class CvMmapClient {
public:
	using OnFrameCallback = std::move_only_function<void(
		const frame_metadata_t &metadata, std::span<const uint8_t> buffer)>;
	using OnFramePlanesCallback = std::move_only_function<void(
		const frame_metadata_t &metadata, frame_planes_view_t planes)>;
	using OnEventCallback = std::move_only_function<void(ModuleStatus status)>;

	static constexpr auto DEFAULT_CONTROL_TIMEOUT =
		std::chrono::milliseconds{1000};

	explicit CvMmapClient(const std::string &instance_name);
	~CvMmapClient();

	CvMmapClient(const CvMmapClient &) = delete;
	CvMmapClient &operator=(const CvMmapClient &) = delete;
	CvMmapClient(CvMmapClient &&) noexcept;
	CvMmapClient &operator=(CvMmapClient &&) noexcept;

	[[nodiscard]]
	const std::string &Name() const;
	void Start();
	void Stop();

	void SetFrameCallback(OnFrameCallback &&cb);
	void SetFramePlanesCallback(OnFramePlanesCallback &&cb);
	void SetEventCallback(OnEventCallback &&cb);

	[[nodiscard]]
	int32_t
	ResetFrameCount(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

private:
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

} // namespace cvmmap
