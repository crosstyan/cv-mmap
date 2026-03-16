#include <cvmmap/nats_client.hpp>
#include <cvmmap/nats_subjects.hpp>
#include <cvmmap/parser.hpp>

#include <nats.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <control.pb.h>

namespace cvmmap {

namespace pb = ::cvmmap::proto;

namespace {

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

int from_proto_error_code(const pb::ErrorCode error_code) {
	switch (error_code) {
	case pb::ERROR_CODE_OK:
		return CONTROL_RESPONSE_OK;
	case pb::ERROR_CODE_UNKNOWN_CMD:
		return CONTROL_RESPONSE_UNKNOWN_CMD;
	case pb::ERROR_CODE_UNSUPPORTED:
		return CONTROL_RESPONSE_UNSUPPORTED;
	case pb::ERROR_CODE_INVALID_PAYLOAD:
		return CONTROL_RESPONSE_INVALID_PAYLOAD;
	case pb::ERROR_CODE_OUT_OF_RANGE:
		return CONTROL_RESPONSE_OUT_OF_RANGE;
	case pb::ERROR_CODE_TIMEOUT:
		return CONTROL_RESPONSE_TIMEOUT;
	default:
		return CONTROL_RESPONSE_ERROR;
	}
}

std::expected<std::string, ControlError> recorder_capabilities_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_capabilities(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_capabilities(target_key);
	default:
		return std::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "recording format is required",
		});
	}
}

std::expected<std::string, ControlError> recording_start_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_start(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_start(target_key);
	default:
		return std::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "recording format is required",
		});
	}
}

std::expected<std::string, ControlError> recording_stop_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_stop(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_stop(target_key);
	default:
		return std::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "recording format is required",
		});
	}
}

std::expected<std::string, ControlError> recording_status_subject(
	const std::string &target_key,
	const RecordingFormat format) {
	switch (format) {
	case RecordingFormat::Svo:
		return nats::subject_control_recorder_svo_status(target_key);
	case RecordingFormat::Mcap:
		return nats::subject_control_recorder_mcap_status(target_key);
	default:
		return std::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
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
	std::expected<RespMsg, int> request(
		const std::string &subject,
		const ReqMsg &request_message,
		const std::chrono::milliseconds timeout) {
		std::lock_guard<std::mutex> lock(conn_mutex);
		if (!conn) {
			return std::unexpected(CONTROL_RESPONSE_ERROR);
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
				return std::unexpected(CONTROL_RESPONSE_TIMEOUT);
			}
			spdlog::error(
				"nats request to '{}': {}",
				subject,
				natsStatus_GetText(status));
			return std::unexpected(CONTROL_RESPONSE_ERROR);
		}

		RespMsg response;
		if (!response.ParseFromArray(natsMsg_GetData(reply), natsMsg_GetDataLength(reply))) {
			natsMsg_Destroy(reply);
			return std::unexpected(CONTROL_RESPONSE_INVALID_PAYLOAD);
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
			int32_t status_code = 0;
			switch (event.status()) {
			case pb::MODULE_STATUS_CODE_ONLINE:
				status_code = MODULE_STATUS_ONLINE;
				break;
			case pb::MODULE_STATUS_CODE_OFFLINE:
				status_code = MODULE_STATUS_OFFLINE;
				break;
			case pb::MODULE_STATUS_CODE_STREAM_RESET:
				status_code = MODULE_STATUS_STREAM_RESET;
				break;
			default:
				break;
			}
			if (status_code != 0) {
				self->on_module_status(status_code);
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

std::expected<int, int> NatsControlClient::ResetFrameCount(
	const std::chrono::milliseconds timeout) {
	pb::ResetFrameCountRequest request;
	auto response =
		pimpl_->request<pb::ResetFrameCountRequest, pb::ResetFrameCountResponse>(
			nats::subject_control_source_reset(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return std::unexpected(response.error());
	}
	return from_proto_error_code(response->error());
}

std::expected<SourceInfo, int> NatsControlClient::GetSourceInfo(
	const std::chrono::milliseconds timeout) {
	pb::GetSourceInfoRequest request;
	auto response =
		pimpl_->request<pb::GetSourceInfoRequest, pb::GetSourceInfoResponse>(
			nats::subject_control_source_info(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return std::unexpected(response.error());
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return std::unexpected(from_proto_error_code(response->error()));
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

std::expected<SeekResult, int> NatsControlClient::SeekTimestampNs(
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
		return std::unexpected(response.error());
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return std::unexpected(from_proto_error_code(response->error()));
	}
	return SeekResult{
		.requested_timestamp_ns = response->requested_timestamp_ns(),
		.landed_timestamp_ns = response->landed_timestamp_ns(),
		.landed_frame_count = response->landed_frame_count(),
		.exact_match = response->exact_match(),
	};
}

std::expected<ControlCapabilities, ControlError> NatsControlClient::GetCapabilities(
	const std::chrono::milliseconds timeout) {
	pb::CapabilitiesRequest request;
	auto source_response =
		pimpl_->request<pb::CapabilitiesRequest, pb::CapabilitiesResponse>(
			nats::subject_control_source_capabilities(pimpl_->target_key),
			request,
			timeout);
	if (!source_response) {
		return std::unexpected(ControlError{.code = source_response.error()});
	}
	if (source_response->error() != pb::ERROR_CODE_OK) {
		return std::unexpected(ControlError{
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

std::expected<RecordingStatus, ControlError> NatsControlClient::StartRecording(
	const RecordingRequest &request,
	const std::chrono::milliseconds timeout) {
	if (request.output_path.empty()) {
		return std::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "recording path is empty",
		});
	}

	auto subject = recording_start_subject(pimpl_->target_key, request.format);
	if (!subject) {
		return std::unexpected(subject.error());
	}

	pb::RecordingStartRequest wire_request;
	wire_request.set_output_path(request.output_path);

	switch (request.format) {
	case RecordingFormat::Svo:
		if (request.mcap_options) {
			return std::unexpected(ControlError{
				.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
				.message = "MCAP options are invalid for SVO recording",
			});
		}
		if (request.svo_options) {
			apply_svo_options(*request.svo_options, wire_request.mutable_svo_options());
		}
		break;
	case RecordingFormat::Mcap:
		if (request.svo_options) {
			return std::unexpected(ControlError{
				.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
				.message = "SVO options are invalid for MCAP recording",
			});
		}
		if (request.mcap_options) {
			apply_mcap_options(*request.mcap_options, wire_request.mutable_mcap_options());
		}
		break;
	default:
		return std::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "recording format is required",
		});
	}

	auto response =
		pimpl_->request<pb::RecordingStartRequest, pb::RecordingStatusResponse>(
			*subject,
			wire_request,
			timeout);
	if (!response) {
		return std::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return std::unexpected(ControlError{
			.code = from_proto_error_code(response->error()),
			.message = response->error_message(),
		});
	}
	return to_recording_status(*response);
}

std::expected<RecordingStatus, ControlError> NatsControlClient::StopRecording(
	const RecordingFormat format,
	const std::chrono::milliseconds timeout) {
	auto subject = recording_stop_subject(pimpl_->target_key, format);
	if (!subject) {
		return std::unexpected(subject.error());
	}

	pb::RecordingStopRequest request;
	auto response =
		pimpl_->request<pb::RecordingStopRequest, pb::RecordingStatusResponse>(
			*subject,
			request,
			timeout);
	if (!response) {
		return std::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return std::unexpected(ControlError{
			.code = from_proto_error_code(response->error()),
			.message = response->error_message(),
		});
	}
	return to_recording_status(*response);
}

std::expected<RecordingStatus, ControlError> NatsControlClient::GetRecordingStatus(
	const RecordingFormat format,
	const std::chrono::milliseconds timeout) {
	auto subject = recording_status_subject(pimpl_->target_key, format);
	if (!subject) {
		return std::unexpected(subject.error());
	}

	pb::RecordingStatusRequest request;
	auto response =
		pimpl_->request<pb::RecordingStatusRequest, pb::RecordingStatusResponse>(
			*subject,
			request,
			timeout);
	if (!response) {
		return std::unexpected(ControlError{.code = response.error()});
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return std::unexpected(ControlError{
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

} // namespace cvmmap
