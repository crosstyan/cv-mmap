#include <bits/types/error_t.h>
#include <opencv2/videoio.hpp>
#include <thread>
#include "app_backends_opencv.hpp"
#include "app_backends_facade.hpp"
#include "app_enum_models.hpp"

namespace app::backends {
struct OpenCVBackendOptions {
	opencv_parameter_t parameter;
	bool looping;
	app::VideoCaptureAPIs api_preference;
};

struct OpenCVBackendImpl {
	OpenCVBackendOptions options;
	cv::VideoCapture cap;
	std::jthread worker_thread;
	on_metadata_fn_t on_metadata{nullptr};
	on_frame_fn_t on_frame{nullptr};
	// TODO: might need some stop token or flag for thread management (worker thread)

	OpenCVBackendImpl() = default;
	OpenCVBackendImpl(OpenCVBackendOptions opts) : options(std::move(opts)) {}

	void Init() {
	}

	void Shutdown() {
	}
	void SetOnMetadata(on_metadata_fn_t on_metadata_) {
		on_metadata = std::move(on_metadata_);
	}
	void SetOnFrame(on_frame_fn_t on_frame_) {
		on_frame = std::move(on_frame_);
	}
	error_t SeekFrame(size_t frame_index) {
		// TODO: implement seek functionality
		return -EOPNOTSUPP;
	}
};

// OpenCVBackend public API

OpenCVBackend::OpenCVBackend(std::variant<int, std::string> parameter,
							 bool looping,
							 app::VideoCaptureAPIs api_preference) : impl(std::make_unique<OpenCVBackendImpl>(OpenCVBackendOptions{
																		 .parameter      = std::move(parameter),
																		 .looping        = looping,
																		 .api_preference = api_preference,
																	 })) {}
OpenCVBackend::~OpenCVBackend() = default;
void OpenCVBackend::Init() {
	impl->Init();
}

void OpenCVBackend::Shutdown() {
	impl->Shutdown();
}

void OpenCVBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void OpenCVBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

error_t OpenCVBackend::SeekFrame(size_t frame_index) {
	return impl->SeekFrame(frame_index);
}
}