#pragma once

#include <cvmmap/nats_client.hpp>
#include <cvmmap/nats_subjects.hpp>

#include <cvmmap/compat/expected.hpp>
#include <nats.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <control.pb.h>

namespace cvmmap {

namespace pb = ::cvmmap::proto;

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
		if (!response.ParseFromArray(
				natsMsg_GetData(reply),
				natsMsg_GetDataLength(reply))) {
			natsMsg_Destroy(reply);
			return cvmmap::unexpected(ControlErrorCode::InvalidPayload);
		}
		natsMsg_Destroy(reply);
		return response;
	}

	void destroy_subscription(natsSubscription *&subscription);
	static void on_body_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure);
	static void on_status_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure);
	void refresh_body_subscription_locked();
	void refresh_status_subscription_locked();
};

} // namespace cvmmap
