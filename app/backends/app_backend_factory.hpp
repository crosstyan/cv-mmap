#pragma once

#include <optional>
#include <string>

#include <cvmmap/compat/expected.hpp>

#include "app_backends_facade.hpp"

namespace app {
struct Config;
}

namespace app::backends {

struct BackendAssembly {
	pro::proxy<IBackend> backend{};
	std::optional<pro::proxy<IBodyTrackingBackend>> body_tracking{};
	std::optional<pro::proxy<ICameraControlBackend>> camera_control{};
	std::optional<pro::proxy<ISvoRecordableBackend>> svo_recordable{};
	std::optional<pro::proxy<IDirectFrameBackend>> direct_frame{};
	std::optional<pro::proxy<IEncodedAccessUnitBackend>> encoded_access_unit{};
};

cvmmap::expected<BackendAssembly, std::string>
MakeBackendAssembly(const app::Config &config);

} // namespace app::backends
