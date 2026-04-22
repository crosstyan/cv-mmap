#ifndef C2500662_3785_423E_AAB2_7ADAA8BE6D16
#define C2500662_3785_423E_AAB2_7ADAA8BE6D16

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <cvmmap/client.hpp>
#include <cvmmap/compat/expected.hpp>
#include <proxy/proxy.h>

namespace app {
struct Config;
}

namespace app::backends {

struct ResolvedPlaylistState {
	std::vector<std::string> paths{};
	bool sort_by_recording_time{false};
	size_t current_index{0};
};

PRO_DEF_MEM_DISPATCH(MemGetConfiguredRequest, GetConfiguredRequest);
PRO_DEF_MEM_DISPATCH(MemSupportsRuntimeApply, SupportsRuntimeApply);
PRO_DEF_MEM_DISPATCH(MemHasPlaylist, HasPlaylist);
PRO_DEF_MEM_DISPATCH(MemSize, Size);
PRO_DEF_MEM_DISPATCH(MemCurrentIndex, CurrentIndex);
PRO_DEF_MEM_DISPATCH(MemSetCurrentIndex, SetCurrentIndex);
PRO_DEF_MEM_DISPATCH(MemCurrentPath, CurrentPath);
PRO_DEF_MEM_DISPATCH(MemApplyCurrentPathToConfig, ApplyCurrentPathToConfig);
PRO_DEF_MEM_DISPATCH(MemGetInfo, GetInfo);
PRO_DEF_MEM_DISPATCH(MemSnapshotState, SnapshotState);
PRO_DEF_MEM_DISPATCH(MemRestoreState, RestoreState);
PRO_DEF_MEM_DISPATCH(MemReplaceState, ReplaceState);
PRO_DEF_MEM_DISPATCH(MemResolve, Resolve);

// clang-format off
struct IPlaylistController : pro::facade_builder
	::add_convention<MemGetConfiguredRequest, std::optional<cvmmap::PlaylistRequest>() const>
	::add_convention<MemSupportsRuntimeApply, bool() const>
	::add_convention<MemHasPlaylist, bool() const>
	::add_convention<MemSize, size_t() const>
	::add_convention<MemCurrentIndex, size_t() const>
	::add_convention<MemSetCurrentIndex, bool(size_t)>
	::add_convention<MemCurrentPath, std::optional<std::string>() const>
	::add_convention<MemApplyCurrentPathToConfig, void()>
	::add_convention<MemGetInfo, cvmmap::PlaylistInfo() const>
	::add_convention<MemSnapshotState, std::optional<ResolvedPlaylistState>() const>
	::add_convention<MemRestoreState, void(std::optional<ResolvedPlaylistState>)>
	::add_convention<MemReplaceState, void(ResolvedPlaylistState)>
	::add_convention<MemResolve, cvmmap::expected<ResolvedPlaylistState, cvmmap::ControlError>(const cvmmap::PlaylistRequest &) const>
	::build {};
// clang-format on

pro::proxy<IPlaylistController> MakePlaylistController(app::Config &config);

} // namespace app::backends

#endif /* C2500662_3785_423E_AAB2_7ADAA8BE6D16 */
