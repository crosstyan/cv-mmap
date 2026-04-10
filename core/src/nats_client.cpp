#include <cvmmap/nats_client.hpp>
#include <cvmmap/nats_subjects.hpp>
#include <cvmmap/parser.hpp>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/format.hpp>
#include <google/protobuf/util/json_util.h>
#include <nats.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <control.pb.h>

namespace cvmmap {

namespace pb = ::cvmmap::proto;
namespace json = ::google::protobuf::util;

namespace {

constexpr std::string_view kNatsMicroServiceName = "cvmmap_producer";

SourceKind from_proto_source_kind(const pb::SourceKind source_kind) {
	switch (source_kind) {
	case pb::SOURCE_KIND_LIVE:
		return SourceKind::Live;
	case pb::SOURCE_KIND_FINITE:
		return SourceKind::Finite;
	default:
		return SourceKind::Unknown;
	}
}

TimestampDomain from_proto_timestamp_domain(const pb::TimestampDomain timestamp_domain) {
	switch (timestamp_domain) {
	case pb::TIMESTAMP_DOMAIN_UNIX_EPOCH_NS:
		return TimestampDomain::UnixEpochNs;
	case pb::TIMESTAMP_DOMAIN_MEDIA_TIME_NS:
		return TimestampDomain::MediaTimeNs;
	default:
		return TimestampDomain::Unknown;
	}
}

RecordingFormat from_proto_recording_format(const pb::RecordingFormat format) {
	switch (format) {
	case pb::RECORDING_FORMAT_SVO:
		return RecordingFormat::Svo;
	case pb::RECORDING_FORMAT_MCAP:
		return RecordingFormat::Mcap;
	default:
		return RecordingFormat::Unknown;
	}
}

ControlErrorCode from_proto_error_code(const pb::ErrorCode error_code) {
	switch (error_code) {
	case pb::ERROR_CODE_OK:
		return ControlErrorCode::Ok;
	case pb::ERROR_CODE_UNKNOWN_CMD:
		return ControlErrorCode::UnknownCmd;
	case pb::ERROR_CODE_UNSUPPORTED:
		return ControlErrorCode::Unsupported;
	case pb::ERROR_CODE_INVALID_PAYLOAD:
		return ControlErrorCode::InvalidPayload;
	case pb::ERROR_CODE_OUT_OF_RANGE:
		return ControlErrorCode::OutOfRange;
	case pb::ERROR_CODE_TIMEOUT:
		return ControlErrorCode::Timeout;
	default:
		return ControlErrorCode::Error;
	}
}

cvmmap::expected<std::string, ControlError> recorder_capabilities_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_capabilities(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_capabilities(target_key);
	default:
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording format is required",
		});
	}
}

cvmmap::expected<std::string, ControlError> recording_start_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_start(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_start(target_key);
	default:
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording format is required",
		});
	}
}

cvmmap::expected<std::string, ControlError> recording_stop_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_stop(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_stop(target_key);
	default:
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording format is required",
		});
	}
}

cvmmap::expected<std::string, ControlError> recording_status_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_status(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_status(target_key);
	default:
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording format is required",
		});
	}
}

RecordingStatus to_recording_status(const pb::RecordingStatusResponse &response) {
	return RecordingStatus{
		.format = from_proto_recording_format(response.format()),
		.can_record = response.can_record(),
		.is_recording = response.is_recording(),
		.is_paused = response.is_paused(),
		.last_frame_ok = response.last_frame_ok(),
		.frames_ingested = response.frames_ingested(),
		.frames_encoded = response.frames_encoded(),
		.active_path = response.active_path(),
	};
}

PlaylistInfo to_playlist_info(const pb::PlaylistInfo &wire_info) {
	PlaylistInfo info{
		.has_playlist = wire_info.has_playlist(),
		.sort_by_recording_time = wire_info.sort_by_recording_time(),
		.current_index = wire_info.current_index(),
		.current_path = wire_info.current_path(),
	};
	info.paths.reserve(static_cast<size_t>(wire_info.paths_size()));
	for (const auto &path : wire_info.paths()) {
		info.paths.push_back(path);
	}
	return info;
}

void merge_recorder_formats(
	ControlCapabilities &capabilities,
	const pb::CapabilitiesResponse &response) {
	for (const auto format : response.available_recording_formats()) {
		const auto decoded = from_proto_recording_format(
			static_cast<pb::RecordingFormat>(format));
		if (decoded == RecordingFormat::Unknown ||
			capabilities.supports_recording_format(decoded)) {
			continue;
		}
		capabilities.available_recording_formats.push_back(decoded);
	}
}

void apply_svo_options(
	const SvoRecordingOptions &options,
	pb::SvoRecordingOptions *wire_options) {
	if (options.compression_mode) {
		wire_options->set_compression_mode(*options.compression_mode);
	}
	if (options.bitrate) {
		wire_options->set_bitrate(*options.bitrate);
	}
	if (options.target_framerate) {
		wire_options->set_target_framerate(*options.target_framerate);
	}
	if (options.transcode_streaming_input) {
		wire_options->set_transcode_streaming_input(*options.transcode_streaming_input);
	}
}

void apply_mcap_options(
	const McapRecordingOptions &options,
	pb::McapRecordingOptions *wire_options) {
	if (options.compression) {
		wire_options->set_compression(*options.compression);
	}
	if (options.topic) {
		wire_options->set_topic(*options.topic);
	}
	if (options.depth_topic) {
		wire_options->set_depth_topic(*options.depth_topic);
	}
	if (options.body_topic) {
		wire_options->set_body_topic(*options.body_topic);
	}
	if (options.frame_id) {
		wire_options->set_frame_id(*options.frame_id);
	}
}

void apply_playlist_request(
	const PlaylistRequest &request,
	pb::ApplyPlaylistRequest *wire_request) {
	wire_request->set_sort_by_recording_time(request.sort_by_recording_time);
	for (const auto &path : request.paths) {
		wire_request->add_paths(path);
	}
}

DiscoveryError make_discovery_error(
	const DiscoveryErrorCode code,
	std::string message) {
	return DiscoveryError{
		.code = code,
		.message = std::move(message),
	};
}

template <typename StatusLike>
std::string status_message_text(const StatusLike &status) {
	const auto message = status.message();
	if constexpr (requires { message.as_string(); }) {
		return std::string(message.as_string());
	} else {
		return std::string(message);
	}
}
DiscoveryError make_nats_discovery_error(
	const natsStatus status,
	std::string subject) {
	const auto code =
		status == NATS_TIMEOUT ?
			DiscoveryErrorCode::Timeout :
			DiscoveryErrorCode::NatsError;
	return make_discovery_error(
		code,
		cvmmap::format(
			"nats discovery request to '{}': {}",
			subject,
			natsStatus_GetText(status)));
}

template <typename Message>
cvmmap::expected<Message, DiscoveryError> parse_discovery_json(
	std::span<const uint8_t> data,
	std::string_view context) {
	Message message;
	const auto json_payload = std::string(
		reinterpret_cast<const char *>(data.data()),
		data.size());
	auto status = json::JsonStringToMessage(json_payload, &message);
	if (!status.ok()) {
		return cvmmap::unexpected(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			cvmmap::format(
				"invalid {} discovery payload: {}",
				context,
				status_message_text(status))));
	}
	return message;
}

std::optional<std::string> metadata_value(
	const google::protobuf::Map<std::string, std::string> &metadata,
	std::string_view key) {
	const auto it = metadata.find(std::string(key));
	if (it == metadata.end()) {
		return std::nullopt;
	}
	return it->second;
}

cvmmap::expected<DiscoveredProducer, DiscoveryError> to_discovered_producer(
	const pb::NatsMicroInfoResponse &info) {
	DiscoveredProducer producer{
		.service_id = info.id(),
		.service_name = info.name(),
		.service_version = info.version(),
	};

	if (producer.service_name != kNatsMicroServiceName) {
		return cvmmap::unexpected(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			cvmmap::format(
				"unexpected discovery service name '{}'",
				producer.service_name)));
	}

	const auto assign_required = [&](std::string_view key, std::string *field) -> bool {
		auto value = metadata_value(info.metadata(), key);
		if (!value || value->empty()) {
			return false;
		}
		*field = std::move(*value);
		return true;
	};

	if (!assign_required("instance_name", &producer.instance_name) ||
		!assign_required("nats_target_key", &producer.nats_target_key) ||
		!assign_required("shm_name", &producer.shm_name) ||
		!assign_required("zmq_addr", &producer.zmq_addr)) {
		return cvmmap::unexpected(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			cvmmap::format(
				"service '{}' is missing required cvmmap transport metadata",
				producer.service_id)));
	}

	if (auto value = metadata_value(info.metadata(), "namespace")) {
		producer.namespace_name = std::move(*value);
	}
	if (auto value = metadata_value(info.metadata(), "ipc_prefix")) {
		producer.ipc_prefix = std::move(*value);
	}
	if (auto value = metadata_value(info.metadata(), "base_name")) {
		producer.base_name = std::move(*value);
	}
	if (auto value = metadata_value(info.metadata(), "body_subject")) {
		producer.body_subject = std::move(*value);
	} else {
		producer.body_subject = nats::subject_body(producer.nats_target_key);
	}
	if (auto value = metadata_value(info.metadata(), "status_subject")) {
		producer.status_subject = std::move(*value);
	} else {
		producer.status_subject = nats::subject_status(producer.nats_target_key);
	}
	if (auto value = metadata_value(info.metadata(), "control_subject_prefix")) {
		producer.control_subject_prefix = std::move(*value);
	} else {
		producer.control_subject_prefix =
			nats::subject_control_prefix(producer.nats_target_key);
	}
	if (auto value = metadata_value(info.metadata(), "backend")) {
		producer.backend = std::move(*value);
	}

	for (const auto &endpoint : info.endpoints()) {
		if (!endpoint.subject().empty()) {
			producer.control_subjects.push_back(endpoint.subject());
		}
	}
	std::sort(producer.control_subjects.begin(), producer.control_subjects.end());
	producer.control_subjects.erase(
		std::unique(
			producer.control_subjects.begin(),
			producer.control_subjects.end()),
		producer.control_subjects.end());

	return producer;
}

bool matches_query(
	const DiscoveredProducer &producer,
	const DiscoveryQuery &query) {
	if (query.instance_name && producer.instance_name != *query.instance_name) {
		return false;
	}
	if (query.nats_target_key &&
		producer.nats_target_key != *query.nats_target_key) {
		return false;
	}
	if (query.backend && producer.backend != *query.backend) {
		return false;
	}
	return true;
}

} // namespace

struct NatsControlClient::impl {
	std::string target_key;
	std::string nats_url;
	natsConnection *conn{nullptr};
	std::mutex conn_mutex;

	natsSubscription *sub_body{nullptr};
	natsSubscription *sub_status{nullptr};
	OnBodyTrackingCallback on_body_tracking{};
	OnBodyTrackingRawCallback on_body_tracking_raw{};
	OnModuleStatusCallback on_module_status{};

	template <typename ReqMsg, typename RespMsg>
	cvmmap::expected<RespMsg, ControlErrorCode> request(
		const std::string &subject,
		const ReqMsg &request_message,
		const std::chrono::milliseconds timeout) {
		std::lock_guard<std::mutex> lock(conn_mutex);
		if (!conn) {
			return cvmmap::unexpected(ControlErrorCode::Error);
		}

		const auto size = request_message.ByteSizeLong();
		std::vector<uint8_t> bytes(size);
		request_message.SerializeToArray(bytes.data(), static_cast<int>(size));

		natsMsg *reply = nullptr;
		const auto status = natsConnection_Request(
			&reply,
			conn,
			subject.c_str(),
			bytes.data(),
			static_cast<int>(bytes.size()),
			timeout.count());
		if (status != NATS_OK) {
			if (status == NATS_TIMEOUT) {
				return cvmmap::unexpected(ControlErrorCode::Timeout);
			}
			spdlog::error(
				"nats request to '{}': {}",
				subject,
				natsStatus_GetText(status));
			return cvmmap::unexpected(ControlErrorCode::Error);
		}

		RespMsg response;
		if (!response.ParseFromArray(natsMsg_GetData(reply), natsMsg_GetDataLength(reply))) {
			natsMsg_Destroy(reply);
			return cvmmap::unexpected(ControlErrorCode::InvalidPayload);
		}
		natsMsg_Destroy(reply);
		return response;
	}

	void destroy_subscription(natsSubscription *&subscription) {
		if (!subscription) {
			return;
		}
		natsSubscription_Unsubscribe(subscription);
		natsSubscription_Destroy(subscription);
		subscription = nullptr;
	}

	static void on_body_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure) {
		auto *self = static_cast<impl *>(closure);
		if (!self->on_body_tracking) {
			natsMsg_Destroy(message);
			return;
		}

		const auto buffer = std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(natsMsg_GetData(message)),
			natsMsg_GetDataLength(message));
		if (self->on_body_tracking_raw) {
			self->on_body_tracking_raw(buffer);
		}
		if (!buffer.empty() && buffer.front() == BODY_TRACKING_MAGIC) {
			auto parsed = parse_body_tracking_message(buffer);
			if (parsed) {
				self->on_body_tracking(*parsed);
			} else {
				spdlog::error("nats body parse error: {}", parsed.error());
			}
		}
		natsMsg_Destroy(message);
	}

	static void on_status_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure) {
		auto *self = static_cast<impl *>(closure);
		if (!self->on_module_status) {
			natsMsg_Destroy(message);
			return;
		}

		pb::ModuleStatusEvent event;
		if (event.ParseFromArray(natsMsg_GetData(message), natsMsg_GetDataLength(message))) {
			auto status = ModuleStatus::Unknown;
			switch (event.status()) {
			case pb::MODULE_STATUS_CODE_ONLINE:
				status = ModuleStatus::Online;
				break;
			case pb::MODULE_STATUS_CODE_OFFLINE:
				status = ModuleStatus::Offline;
				break;
			case pb::MODULE_STATUS_CODE_STREAM_RESET:
				status = ModuleStatus::StreamReset;
				break;
			default:
				break;
			}
			if (status != ModuleStatus::Unknown) {
				self->on_module_status(status);
			}
		}

		natsMsg_Destroy(message);
	}

	void refresh_body_subscription_locked() {
		if (!conn) {
			return;
		}
		if (!on_body_tracking && !on_body_tracking_raw) {
			destroy_subscription(sub_body);
			return;
		}
		if (sub_body) {
			return;
		}
		const auto subject = nats::subject_body(target_key);
		const auto status = natsConnection_Subscribe(
			&sub_body,
			conn,
			subject.c_str(),
			impl::on_body_msg,
			this);
		if (status != NATS_OK) {
			spdlog::error(
				"nats subscribe to '{}': {}",
				subject,
				natsStatus_GetText(status));
		}
	}

	void refresh_status_subscription_locked() {
		if (!conn) {
			return;
		}
		if (!on_module_status) {
			destroy_subscription(sub_status);
			return;
		}
		if (sub_status) {
			return;
		}
		const auto subject = nats::subject_status(target_key);
		const auto status = natsConnection_Subscribe(
			&sub_status,
			conn,
			subject.c_str(),
			impl::on_status_msg,
			this);
		if (status != NATS_OK) {
			spdlog::error(
				"nats subscribe to '{}': {}",
				subject,
				natsStatus_GetText(status));
		}
	}
};

NatsControlClient::NatsControlClient(std::string target_key, std::string nats_url)
	: pimpl_(std::make_unique<impl>()) {
	pimpl_->target_key = std::move(target_key);
	pimpl_->nats_url = std::move(nats_url);
}

NatsControlClient::~NatsControlClient() {
	Stop();
}

bool NatsControlClient::Start() {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	if (pimpl_->conn) {
		return true;
	}

	natsOptions *options = nullptr;
	natsOptions_Create(&options);
	natsOptions_SetURL(options, pimpl_->nats_url.c_str());

	const auto status = natsConnection_Connect(&pimpl_->conn, options);
	natsOptions_Destroy(options);
	if (status != NATS_OK) {
		spdlog::error(
			"nats client connect to '{}': {}",
			pimpl_->nats_url,
			natsStatus_GetText(status));
		return false;
	}

	pimpl_->refresh_body_subscription_locked();
	pimpl_->refresh_status_subscription_locked();

	spdlog::info("nats client started for target '{}'", pimpl_->target_key);
	return true;
}

void NatsControlClient::Stop() {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->destroy_subscription(pimpl_->sub_body);
	pimpl_->destroy_subscription(pimpl_->sub_status);

	if (pimpl_->conn) {
		natsConnection_Close(pimpl_->conn);
		natsConnection_Destroy(pimpl_->conn);
		pimpl_->conn = nullptr;
	}
}

ControlErrorCode NatsControlClient::ResetFrameCount(
	const std::chrono::milliseconds timeout) {
	pb::ResetFrameCountRequest request;
	auto response =
		pimpl_->request<pb::ResetFrameCountRequest, pb::ResetFrameCountResponse>(
			nats::subject_control_source_reset(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return response.error();
	}
	return from_proto_error_code(response->error());
}

cvmmap::expected<SourceInfo, ControlErrorCode> NatsControlClient::GetSourceInfo(
	const std::chrono::milliseconds timeout) {
	pb::GetSourceInfoRequest request;
	auto response =
		pimpl_->request<pb::GetSourceInfoRequest, pb::GetSourceInfoResponse>(
			nats::subject_control_source_info(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response.error());
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(from_proto_error_code(response->error()));
	}
	return SourceInfo{
		.source_kind = from_proto_source_kind(response->source_kind()),
		.timestamp_domain = from_proto_timestamp_domain(response->timestamp_domain()),
		.flags = response->flags(),
		.timeline_start_ns = response->timeline_start_ns(),
		.timeline_end_ns = response->timeline_end_ns(),
		.duration_ns = response->duration_ns(),
		.current_timestamp_ns = response->current_timestamp_ns(),
		.current_frame_count = response->current_frame_count(),
	};
}

cvmmap::expected<SeekResult, ControlErrorCode> NatsControlClient::SeekTimestampNs(
	const uint64_t timestamp_ns,
	const std::chrono::milliseconds timeout) {
	pb::SeekTimestampRequest request;
	request.set_target_timestamp_ns(timestamp_ns);
	auto response =
		pimpl_->request<pb::SeekTimestampRequest, pb::SeekTimestampResponse>(
			nats::subject_control_source_seek(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response.error());
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(from_proto_error_code(response->error()));
	}
	return SeekResult{
		.requested_timestamp_ns = response->requested_timestamp_ns(),
		.landed_timestamp_ns = response->landed_timestamp_ns(),
		.landed_frame_count = response->landed_frame_count(),
		.exact_match = response->exact_match(),
	};
}

cvmmap::expected<PlaylistInfo, ControlError> NatsControlClient::ApplyPlaylist(
	const PlaylistRequest &request,
	const std::chrono::milliseconds timeout) {
	pb::ApplyPlaylistRequest wire_request;
	apply_playlist_request(request, &wire_request);
	auto response =
		pimpl_->request<pb::ApplyPlaylistRequest, pb::ApplyPlaylistResponse>(
			nats::subject_control_source_playlist_apply(pimpl_->target_key),
			wire_request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(ControlError{
			.code = from_proto_error_code(response->error()),
			.message = response->error_message(),
		});
	}
	return to_playlist_info(response->playlist_info());
}

cvmmap::expected<PlaylistInfo, ControlError> NatsControlClient::GetPlaylistInfo(
	const std::chrono::milliseconds timeout) {
	pb::GetPlaylistInfoRequest request;
	auto response =
		pimpl_->request<pb::GetPlaylistInfoRequest, pb::GetPlaylistInfoResponse>(
			nats::subject_control_source_playlist_info(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(ControlError{
			.code = from_proto_error_code(response->error()),
			.message = response->error_message(),
		});
	}
	return to_playlist_info(response->playlist_info());
}

cvmmap::expected<ControlCapabilities, ControlError> NatsControlClient::GetCapabilities(
	const std::chrono::milliseconds timeout) {
	pb::CapabilitiesRequest request;
	auto source_response =
		pimpl_->request<pb::CapabilitiesRequest, pb::CapabilitiesResponse>(
			nats::subject_control_source_capabilities(pimpl_->target_key),
			request,
			timeout);
	if (!source_response) {
		return cvmmap::unexpected(ControlError{.code = source_response.error()});
	}
	if (source_response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(ControlError{
			.code = from_proto_error_code(source_response->error()),
		});
	}

	ControlCapabilities capabilities{
		.can_seek = source_response->can_seek(),
	};

	auto merge_recorder_capability = [&](const RecordingFormat format) {
		auto subject = recorder_capabilities_subject(pimpl_->target_key, format);
		if (!subject) {
			return;
		}
		auto response =
			pimpl_->request<pb::CapabilitiesRequest, pb::CapabilitiesResponse>(
				*subject,
				request,
				timeout);
		if (!response || response->error() != pb::ERROR_CODE_OK) {
			return;
		}
		merge_recorder_formats(capabilities, *response);
	};

	merge_recorder_capability(RecordingFormat::Svo);
	merge_recorder_capability(RecordingFormat::Mcap);
	return capabilities;
}

cvmmap::expected<RecordingStatus, ControlError> NatsControlClient::StartRecording(
	const RecordingRequest &request,
	const std::chrono::milliseconds timeout) {
	if (request.output_path.empty()) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording path is empty",
		});
	}

	auto subject = recording_start_subject(pimpl_->target_key, request.format);
	if (!subject) {
		return cvmmap::unexpected(subject.error());
	}

	pb::RecordingStartRequest wire_request;
	wire_request.set_output_path(request.output_path);

	switch (request.format) {
	case RecordingFormat::Svo:
		if (request.mcap_options) {
			return cvmmap::unexpected(ControlError{
				.code = ControlErrorCode::InvalidPayload,
				.message = "MCAP options are invalid for SVO recording",
			});
		}
		if (request.svo_options) {
			apply_svo_options(*request.svo_options, wire_request.mutable_svo_options());
		}
		break;
	case RecordingFormat::Mcap:
		if (request.svo_options) {
			return cvmmap::unexpected(ControlError{
				.code = ControlErrorCode::InvalidPayload,
				.message = "SVO options are invalid for MCAP recording",
			});
		}
		if (request.mcap_options) {
			apply_mcap_options(*request.mcap_options, wire_request.mutable_mcap_options());
		}
		break;
	default:
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording format is required",
		});
	}

	auto response =
		pimpl_->request<pb::RecordingStartRequest, pb::RecordingStatusResponse>(
			*subject,
			wire_request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(ControlError{
			.code = from_proto_error_code(response->error()),
			.message = response->error_message(),
		});
	}
	return to_recording_status(*response);
}

cvmmap::expected<RecordingStatus, ControlError> NatsControlClient::StopRecording(
	const RecordingFormat format,
	const std::chrono::milliseconds timeout) {
	auto subject = recording_stop_subject(pimpl_->target_key, format);
	if (!subject) {
		return cvmmap::unexpected(subject.error());
	}

	pb::RecordingStopRequest request;
	auto response =
		pimpl_->request<pb::RecordingStopRequest, pb::RecordingStatusResponse>(
			*subject,
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(ControlError{
			.code = from_proto_error_code(response->error()),
			.message = response->error_message(),
		});
	}
	return to_recording_status(*response);
}

cvmmap::expected<RecordingStatus, ControlError> NatsControlClient::GetRecordingStatus(
	const RecordingFormat format,
	const std::chrono::milliseconds timeout) {
	auto subject = recording_status_subject(pimpl_->target_key, format);
	if (!subject) {
		return cvmmap::unexpected(subject.error());
	}

	pb::RecordingStatusRequest request;
	auto response =
		pimpl_->request<pb::RecordingStatusRequest, pb::RecordingStatusResponse>(
			*subject,
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(ControlError{
			.code = from_proto_error_code(response->error()),
			.message = response->error_message(),
		});
	}
	return to_recording_status(*response);
}

void NatsControlClient::SetBodyTrackingCallback(OnBodyTrackingCallback &&callback) {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->on_body_tracking = std::move(callback);
	pimpl_->refresh_body_subscription_locked();
}

void NatsControlClient::SetBodyTrackingRawCallback(
	OnBodyTrackingRawCallback &&callback) {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->on_body_tracking_raw = std::move(callback);
	pimpl_->refresh_body_subscription_locked();
}

void NatsControlClient::SetModuleStatusCallback(
	OnModuleStatusCallback &&callback) {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->on_module_status = std::move(callback);
	pimpl_->refresh_status_subscription_locked();
}

cvmmap::expected<std::vector<DiscoveredProducer>, DiscoveryError>
DiscoverCvMmapProducers(
	const DiscoveryRequest &request,
	const std::chrono::milliseconds timeout) {
	auto nats_url = request.nats_url.value_or(
		std::string(CvMmapClient::DEFAULT_NATS_URL));

	natsOptions *options = nullptr;
	natsConnection *conn = nullptr;
	natsInbox *inbox = nullptr;
	natsSubscription *sub = nullptr;

	const auto cleanup = [&]() {
		if (sub) {
			natsSubscription_Unsubscribe(sub);
			natsSubscription_Destroy(sub);
			sub = nullptr;
		}
		if (inbox) {
			natsInbox_Destroy(inbox);
			inbox = nullptr;
		}
		if (conn) {
			natsConnection_Close(conn);
			natsConnection_Destroy(conn);
			conn = nullptr;
		}
		if (options) {
			natsOptions_Destroy(options);
			options = nullptr;
		}
	};

	auto fail = [&](DiscoveryError error)
		-> cvmmap::expected<std::vector<DiscoveredProducer>, DiscoveryError> {
		cleanup();
		return cvmmap::unexpected(std::move(error));
	};

	natsOptions_Create(&options);
	natsOptions_SetURL(options, nats_url.c_str());
	auto status = natsConnection_Connect(&conn, options);
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, nats_url));
	}

	status = natsInbox_Create(&inbox);
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, "$SRV.PING"));
	}

	status = natsConnection_SubscribeSync(
		&sub,
		conn,
		reinterpret_cast<const char *>(inbox));
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, "$SRV.PING"));
	}

	const auto ping_subject =
		cvmmap::format("$SRV.PING.{}", kNatsMicroServiceName);
	status = natsConnection_PublishRequest(
		conn,
		ping_subject.c_str(),
		reinterpret_cast<const char *>(inbox),
		nullptr,
		0);
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, ping_subject));
	}

	std::set<std::string> service_ids;
	size_t malformed_ping_count = 0;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (true) {
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			break;
		}

		const auto remaining_ms =
			std::max<int64_t>(
				1,
				std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - now)
					.count());
		natsMsg *message = nullptr;
		status = natsSubscription_NextMsg(
			&message,
			sub,
			static_cast<int64_t>(remaining_ms));
		if (status == NATS_TIMEOUT) {
			break;
		}
		if (status != NATS_OK) {
			return fail(make_nats_discovery_error(status, ping_subject));
		}

		const auto payload = std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(natsMsg_GetData(message)),
			static_cast<size_t>(natsMsg_GetDataLength(message)));
		auto parsed = parse_discovery_json<pb::NatsMicroPingResponse>(
			payload,
			"ping");
		natsMsg_Destroy(message);
		if (!parsed) {
			++malformed_ping_count;
			spdlog::warn("{}", parsed.error().message);
			continue;
		}
		if (parsed->name() == kNatsMicroServiceName && !parsed->id().empty()) {
			service_ids.insert(parsed->id());
		}
	}

	std::vector<DiscoveredProducer> producers;
	size_t malformed_info_count = 0;
	for (const auto &service_id : service_ids) {
		const auto info_subject =
			cvmmap::format("$SRV.INFO.{}.{}", kNatsMicroServiceName, service_id);
		natsMsg *reply = nullptr;
		status = natsConnection_Request(
			&reply,
			conn,
			info_subject.c_str(),
			nullptr,
			0,
			timeout.count());
		if (status != NATS_OK) {
			cleanup();
			return cvmmap::unexpected(make_nats_discovery_error(status, info_subject));
		}

		const auto payload = std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(natsMsg_GetData(reply)),
			static_cast<size_t>(natsMsg_GetDataLength(reply)));
		auto parsed_info = parse_discovery_json<pb::NatsMicroInfoResponse>(
			payload,
			"info");
		natsMsg_Destroy(reply);
		if (!parsed_info) {
			++malformed_info_count;
			spdlog::warn("{}", parsed_info.error().message);
			continue;
		}

		auto producer = to_discovered_producer(*parsed_info);
		if (!producer) {
			++malformed_info_count;
			spdlog::warn("{}", producer.error().message);
			continue;
		}
		if (matches_query(*producer, request.query)) {
			producers.push_back(std::move(*producer));
		}
	}

	if (producers.empty() &&
		(service_ids.empty() ? malformed_ping_count : malformed_info_count) > 0) {
		return fail(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			"service discovery received malformed cvmmap discovery payloads"));
	}

	cleanup();
	return producers;
}

} // namespace cvmmap
