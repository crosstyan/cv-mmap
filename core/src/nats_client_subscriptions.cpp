#include "nats_client_internal.hpp"

#include <cvmmap/parser.hpp>

#include <spdlog/spdlog.h>

namespace cvmmap {

void NatsControlClient::impl::destroy_subscription(
	natsSubscription *&subscription) {
	if (!subscription) {
		return;
	}
	natsSubscription_Unsubscribe(subscription);
	natsSubscription_Destroy(subscription);
	subscription = nullptr;
}

void NatsControlClient::impl::on_body_msg(
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
			spdlog::error("bad NATS body parse: {}", parsed.error());
		}
	}
	natsMsg_Destroy(message);
}

void NatsControlClient::impl::on_status_msg(
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

void NatsControlClient::impl::refresh_body_subscription_locked() {
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

void NatsControlClient::impl::refresh_status_subscription_locked() {
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

} // namespace cvmmap
