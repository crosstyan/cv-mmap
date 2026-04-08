#include <cvmmap/client.hpp>
#include <cvmmap/nats_client.hpp>
#include <cvmmap/parser.hpp>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cvmmap/compat/expected.hpp>
#include <fcntl.h>
#include <cvmmap/compat/format.hpp>
#include <memory>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <zmq.hpp>

namespace app {
zmq::context_t &global_zmq_context() {
  static zmq::context_t ctx{1};
  return ctx;
}
} // namespace app

namespace cvmmap {
constexpr size_t CV_MMAP_MAGIC_LEN = frame_metadata_t::CV_MMAP_MAGIC.size();
static_assert(CV_MMAP_MAGIC_LEN == 8);
constexpr std::string_view NATS_DISABLED_MESSAGE =
    "NATS is disabled for this client";

namespace {
ControlError make_control_error(int32_t code, std::span<const uint8_t> payload = {}) {
  auto message = std::string{};
  if (!payload.empty()) {
    const auto *begin = reinterpret_cast<const char *>(payload.data());
    message.assign(begin, begin + payload.size());
  }
  return ControlError{
      .code = code,
      .message = std::move(message),
  };
}

ControlError make_control_error(int32_t code, std::string_view message) {
  return ControlError{
      .code = code,
      .message = std::string(message),
  };
}

ControlError make_nats_disabled_error() {
  return make_control_error(CONTROL_RESPONSE_UNSUPPORTED, NATS_DISABLED_MESSAGE);
}

DiscoveryError make_discovery_error(
    const DiscoveryErrorCode code, std::string message) {
  return DiscoveryError{
      .code = code,
      .message = std::move(message),
  };
}

} // namespace

bool ControlCapabilities::supports_recording_format(
    const RecordingFormat format) const {
  return std::find(available_recording_formats.begin(),
                   available_recording_formats.end(),
                   format) != available_recording_formats.end();
}

struct SharedBuffer {
  SharedBuffer(int shm_fd) {
    shm_fd_ = shm_fd;
    if (shm_fd_ < 0) {
      throw std::invalid_argument("invalid shared memory file descriptor");
    }

    struct stat sb;
    if (fstat(shm_fd_, &sb) < 0) {
      throw std::runtime_error(
          cvmmap::format("fstat {}({})", errno, strerror(errno)));
    }
    shm_size_ = sb.st_size;

    shm_ptr_ = static_cast<uint8_t *>(
        mmap(nullptr, shm_size_, PROT_READ, MAP_SHARED, shm_fd_, 0));
    if (shm_ptr_ == MAP_FAILED) {
      throw std::runtime_error(
          cvmmap::format("mmap {}({})", errno, strerror(errno)));
    }

    // Metadata starts at offset 0 (includes magic)
    metadata_data_ = static_cast<uint8_t *>(shm_ptr_);
    image_data_ = static_cast<uint8_t *>(shm_ptr_) + SHM_PAYLOAD_OFFSET;
  };

  ~SharedBuffer() {
    if (shm_ptr_) {
      munmap(shm_ptr_, shm_size_);
    }
    if (shm_fd_ >= 0) {
      close(shm_fd_);
    }
  };

  cvmmap::expected<std::monostate, std::string> verify() {
    if (shm_size_ < SHM_PAYLOAD_OFFSET) {
      return cvmmap::unexpected(cvmmap::format("shared memory too small: {} < {}",
                                         shm_size_, SHM_PAYLOAD_OFFSET));
    }

    const auto &metadata =
        *reinterpret_cast<const frame_metadata_t *>(metadata_data_);
    if (std::equal(metadata.magic, metadata.magic + CV_MMAP_MAGIC_LEN,
                   frame_metadata_t::CV_MMAP_MAGIC.data()) == false) {
      return cvmmap::unexpected("invalid magic");
    }

    if (metadata.versions_major == 0 && metadata.versions_minor == 0) {
      return {std::monostate{}};
    }

    if (metadata.versions_major != FRAME_METADATA_V1_MAJOR &&
        metadata.versions_major != FRAME_METADATA_V2_MAJOR) {
      return cvmmap::unexpected(cvmmap::format(
          "incompatible major version; got {}.{}, expected {}.x or {}.x",
          metadata.versions_major, metadata.versions_minor,
          FRAME_METADATA_V1_MAJOR, FRAME_METADATA_V2_MAJOR));
    }

    auto parsed = parse_frame_metadata_regions(
        std::span<const uint8_t>(metadata_data_, SHM_PAYLOAD_OFFSET),
        std::span<const uint8_t>(image_data_, shm_size_ - SHM_PAYLOAD_OFFSET));
    if (!parsed) {
      return cvmmap::unexpected(parsed.error());
    }

    normalized_metadata_ = parsed->normalized_metadata;
    left_plane_ = parsed->left_plane;
    depth_unit_ = parsed->depth_unit;
    depth_info_ = parsed->depth_info;
    depth_plane_ = parsed->depth_plane;
    confidence_info_ = parsed->confidence_info;
    confidence_plane_ = parsed->confidence_plane;
    encoded_codec_ = parsed->encoded_codec;
    encoded_bitstream_format_ = parsed->encoded_bitstream_format;
    encoded_flags_ = parsed->encoded_flags;
    encoded_frame_rate_num_ = parsed->encoded_frame_rate_num;
    encoded_frame_rate_den_ = parsed->encoded_frame_rate_den;
    encoded_stream_pts_ns_ = parsed->encoded_stream_pts_ns;
    encoded_access_unit_ = parsed->encoded_access_unit;
    return {std::monostate{}};
  }

  std::span<const uint8_t> image_data() const { return left_plane_; }

  const frame_metadata_t &metadata() const { return normalized_metadata_; }

  frame_planes_view_t planes() const {
    return frame_planes_view_t{
        .left = left_plane_,
        .depth_unit = depth_unit_,
        .depth_info = depth_info_,
        .depth = depth_plane_,
        .confidence_info = confidence_info_,
        .confidence = confidence_plane_,
        .encoded_codec = encoded_codec_,
        .encoded_bitstream_format = encoded_bitstream_format_,
        .encoded_flags = encoded_flags_,
        .encoded_frame_rate_num = encoded_frame_rate_num_,
        .encoded_frame_rate_den = encoded_frame_rate_den_,
        .encoded_stream_pts_ns = encoded_stream_pts_ns_,
        .encoded_access_unit = encoded_access_unit_,
    };
  }

private:
  int shm_fd_{};
  uint8_t *shm_ptr_;
  size_t shm_size_;
  uint8_t *metadata_data_;
  uint8_t *image_data_;
  frame_metadata_t normalized_metadata_{};
  std::span<const uint8_t> left_plane_{};
  DepthUnit depth_unit_{DepthUnit::Unknown};
  std::optional<frame_info_t> depth_info_{};
  std::span<const uint8_t> depth_plane_{};
  std::optional<frame_info_t> confidence_info_{};
  std::span<const uint8_t> confidence_plane_{};
  EncodedCodec encoded_codec_{EncodedCodec::Unknown};
  EncodedBitstreamFormat encoded_bitstream_format_{
      EncodedBitstreamFormat::Unknown};
  uint16_t encoded_flags_{0};
  uint16_t encoded_frame_rate_num_{0};
  uint16_t encoded_frame_rate_den_{0};
  uint64_t encoded_stream_pts_ns_{0};
  std::span<const uint8_t> encoded_access_unit_{};
};

struct CvMmapClient::impl {
  /**
   * inputs, expected to be filled
   */

  std::string instance_name{};
  std::string shm_name{};
  std::string zmq_addr{};
  bool enable_nats = true;
  std::unique_ptr<NatsControlClient> nats_client{};

  bool has_init = false;
  std::atomic_bool is_running = false;

  zmq::socket_t socket{};

  std::unique_ptr<SharedBuffer> shared_buffer{};
  CvMmapClient::OnFrameCallback on_frame_callback{};
  CvMmapClient::OnFramePlanesCallback on_frame_planes_callback{};
  CvMmapClient::OnBodyTrackingCallback on_body_tracking_callback{};
  CvMmapClient::OnEventCallback on_event_callback{};

  std::unique_ptr<std::thread> polling_thread{};

  /** end of properties */

  /** forwarded method */

  /**
   * @brief initialize the client
   * @note use exception
   */
  void init();
  void start();
  void stop();
  void sync_nats_callbacks();

  void polling_task_();
};

void CvMmapClient::impl::init() {
  if (has_init) {
    return;
  }
  // Frame sync always uses ZMQ PUB/SUB
  socket = zmq::socket_t(app::global_zmq_context(), zmq::socket_type::sub);
  has_init = true;
}

void CvMmapClient::impl::start() {
  if (is_running.load(std::memory_order_acquire)) {
    return;
  }
  // Frame sync always uses ZMQ PUB/SUB
  socket.set(zmq::sockopt::conflate_t{}, true);
  socket.set(zmq::sockopt::rcvtimeo_t{}, 100); // 100ms timeout
  socket.connect(zmq_addr);
  socket.set(zmq::sockopt::subscribe_t{}, "");

  if (enable_nats) {
    sync_nats_callbacks();
    if (!nats_client || !nats_client->Start()) {
      throw std::runtime_error("failed to start NATS control client");
    } else {
      spdlog::debug("cvmmap client NATS control/body connected");
    }
  } else {
    spdlog::debug("cvmmap client started without NATS");
  }

  is_running.store(true, std::memory_order_release);
  polling_thread =
      std::make_unique<std::thread>(&CvMmapClient::impl::polling_task_, this);
  if (not polling_thread) {
    is_running.store(false, std::memory_order_release);
    throw std::runtime_error("failed to create thread");
  }
}

void CvMmapClient::impl::sync_nats_callbacks() {
  if (!nats_client) {
    return;
  }

  if (on_body_tracking_callback) {
    nats_client->SetBodyTrackingCallback(
        [this](const body_tracking_frame_t &frame) {
          if (on_body_tracking_callback) {
            on_body_tracking_callback(frame);
          }
        });
  } else {
    nats_client->SetBodyTrackingCallback({});
  }

  if (on_event_callback) {
    nats_client->SetModuleStatusCallback(
        [this](int32_t status_code) {
          if (on_event_callback) {
            on_event_callback(static_cast<ModuleStatus>(status_code));
          }
        });
  } else {
    nats_client->SetModuleStatusCallback({});
  }
}

void CvMmapClient::impl::stop() {
  if (not has_init || not is_running.load(std::memory_order_acquire)) {
    return;
  }

  // Stop NATS client if active
  if (nats_client) {
    nats_client->Stop();
  }

  // Signal the thread to stop
  is_running.store(false, std::memory_order_release);

  if (polling_thread && polling_thread->joinable()) {
    polling_thread->join();
    polling_thread.reset();
  }
}

void CvMmapClient::impl::polling_task_() {
  while (is_running.load(std::memory_order_acquire)) {
    std::array<zmq::pollitem_t, 1> poll_items{{
        {socket.handle(), 0, ZMQ_POLLIN, 0},
    }};
    try {
      zmq::poll(poll_items, std::chrono::milliseconds{100});
    } catch (const zmq::error_t &e) {
      spdlog::error("client poll error: {}", e.what());
      continue;
    }

    if (!(poll_items[0].revents & ZMQ_POLLIN)) {
      continue;
    }

    auto message = zmq::message_t{};
    auto res = socket.recv(message, zmq::recv_flags::dontwait);
    if (not res) {
      continue;
    }
    const auto buf =
        std::span<const uint8_t>(static_cast<uint8_t *>(message.data()), *res);

    if (buf.empty()) {
      continue;
    }

    const uint8_t magic = buf[0];

    // Handle frame sync messages
    if (magic == FRAME_TOPIC_MAGIC) {
      if (buf.size() < sizeof(sync_message_t)) {
        spdlog::error("unexpected `sync_message` size {}", buf.size());
        continue;
      }
      const auto &sync_msg =
          *reinterpret_cast<const sync_message_t *>(buf.data());
      std::string_view label = sync_msg.label();
      if (label != instance_name) {
        spdlog::debug("sync label mismatch: {} != {}", label, instance_name);
        continue;
      }

      if (not shared_buffer) {
        // note that `mode` argument is meaningless for read-only
        spdlog::debug("opening shared memory: {}", shm_name);
        auto fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
        if (fd < 0) {
          spdlog::error("shm_open name={}; {}({})", shm_name, errno,
                        strerror(errno));
          continue;
        }
        try {
          shared_buffer = std::make_unique<SharedBuffer>(fd);
          if (auto verify_res = shared_buffer->verify(); not verify_res) {
            spdlog::error("bad shared buffer: {}", verify_res.error());
            shared_buffer.reset();
            continue;
          }

          const auto &metadata = shared_buffer->metadata();
          if (metadata.versions_major == 0 && metadata.versions_minor == 0) {
            spdlog::warn("shared buffer metadata version is 0.0; accepting "
                         "legacy-compatible layout");
          } else if (metadata.versions_major == FRAME_METADATA_V1_MAJOR &&
                     metadata.versions_minor != VERSION_MINOR) {
            spdlog::warn("shared buffer v1 minor version mismatch: got {}.{}, "
                         "expected {}.{}; continuing",
                         metadata.versions_major, metadata.versions_minor,
                         FRAME_METADATA_V1_MAJOR, VERSION_MINOR);
          } else if (metadata.versions_major == FRAME_METADATA_V2_MAJOR &&
                     metadata.versions_minor != 0) {
            spdlog::warn("shared buffer v2 minor version is {}.{}; continuing "
                         "with parser checks",
                         metadata.versions_major, metadata.versions_minor);
          }
        } catch (const std::exception &e) {
          spdlog::error("SharedBuffer ctor {}", e.what());
          close(fd);
          continue;
        }
      }

      if (shared_buffer) {
        if (auto verify_res = shared_buffer->verify(); not verify_res) {
          spdlog::error("bad shared buffer during frame poll: {}",
                        verify_res.error());
          shared_buffer.reset();
          continue;
        }
      }

      if (shared_buffer && on_frame_callback) {
        on_frame_callback(shared_buffer->metadata(),
                          shared_buffer->image_data());
      }
      if (shared_buffer && on_frame_planes_callback) {
        on_frame_planes_callback(shared_buffer->metadata(),
                                 shared_buffer->planes());
      }
      continue;
    }

    spdlog::warn("unknown message magic 0x{:02x}", magic);
  }

  // out of the loop, clean up
  if (shared_buffer) {
    shared_buffer.reset();
  }
}

CvMmapClient::CvMmapClient(const std::string &instance_name)
    : pimpl_(std::make_unique<impl>()) {
  auto resolved = resolve_cvmmap_target_or_throw(instance_name);
  pimpl_->instance_name = resolved.instance;
  pimpl_->shm_name = resolved.shm_name;
  pimpl_->zmq_addr = resolved.zmq_addr;
  pimpl_->enable_nats = true;
  pimpl_->nats_client = std::make_unique<NatsControlClient>(
      resolved.nats_target_key, std::string(CvMmapClient::DEFAULT_NATS_URL));
  pimpl_->init();
}

CvMmapClient::CvMmapClient(const ClientConfig &config)
    : pimpl_(std::make_unique<impl>()) {
  auto resolved = resolve_cvmmap_target_or_throw(config.instance_name);
  pimpl_->instance_name = resolved.instance;
  pimpl_->shm_name = resolved.shm_name;
  pimpl_->zmq_addr = resolved.zmq_addr;
  pimpl_->enable_nats = config.enable_nats;
  if (pimpl_->enable_nats) {
    pimpl_->nats_client = std::make_unique<NatsControlClient>(
        resolved.nats_target_key,
        config.nats_url.value_or(std::string(CvMmapClient::DEFAULT_NATS_URL)));
  }
  pimpl_->init();
}

CvMmapClient::CvMmapClient() : pimpl_(std::make_unique<impl>()) {}

CvMmapClient::~CvMmapClient() {
  if (pimpl_) {
    pimpl_->stop();
  }
}

CvMmapClient::CvMmapClient(CvMmapClient &&other) noexcept
    : pimpl_(std::move(other.pimpl_)) {
  // other.pimpl_ is now nullptr, which is fine
}

// Move assignment operator
CvMmapClient &CvMmapClient::operator=(CvMmapClient &&other) noexcept {
  if (this != &other) {
    // Stop current instance if it exists
    if (pimpl_) {
      pimpl_->stop();
    }

    // Move the implementation and callback
    pimpl_ = std::move(other.pimpl_);

    // other.pimpl_ is now nullptr, which is fine
  }
  return *this;
}

cvmmap::expected<CvMmapClient, DiscoveryError>
CvMmapClient::ConnectDiscovered(const DiscoveredProducer &producer,
                                const bool enable_nats,
                                std::optional<std::string> nats_url) {
  if (producer.instance_name.empty() || producer.shm_name.empty() ||
      producer.zmq_addr.empty()) {
    return cvmmap::unexpected(make_discovery_error(
        DiscoveryErrorCode::InvalidPayload,
        "discovered producer is missing required transport fields"));
  }
  if (enable_nats && producer.nats_target_key.empty()) {
    return cvmmap::unexpected(make_discovery_error(
        DiscoveryErrorCode::InvalidPayload,
        "discovered producer is missing nats_target_key"));
  }

  CvMmapClient client;
  client.pimpl_->instance_name = producer.instance_name;
  client.pimpl_->shm_name = producer.shm_name;
  client.pimpl_->zmq_addr = producer.zmq_addr;
  client.pimpl_->enable_nats = enable_nats;
  if (enable_nats) {
    client.pimpl_->nats_client = std::make_unique<NatsControlClient>(
        producer.nats_target_key,
        nats_url.value_or(std::string(CvMmapClient::DEFAULT_NATS_URL)));
  }
  client.pimpl_->init();
  return client;
}

cvmmap::expected<CvMmapClient, DiscoveryError>
CvMmapClient::ConnectDiscovered(const DiscoveryConnectConfig &config,
                                const std::chrono::milliseconds timeout) {
  auto discovered = DiscoverCvMmapProducers(
      DiscoveryRequest{
          .query = config.query,
          .nats_url = config.nats_url,
      },
      timeout);
  if (!discovered) {
    return cvmmap::unexpected(discovered.error());
  }
  if (discovered->empty()) {
    return cvmmap::unexpected(make_discovery_error(
        DiscoveryErrorCode::NotFound,
        "no cvmmap producer matched the discovery query"));
  }
  if (discovered->size() != 1) {
    auto details = std::string{};
    for (size_t i = 0; i < discovered->size(); ++i) {
      if (i != 0) {
        details += ", ";
      }
      details += discovered->at(i).instance_name.empty()
                     ? discovered->at(i).service_id
                     : discovered->at(i).instance_name;
    }
    return cvmmap::unexpected(make_discovery_error(
        DiscoveryErrorCode::Ambiguous,
        cvmmap::format(
            "discovery query matched {} producers: {}",
            discovered->size(), details)));
  }
  return ConnectDiscovered(discovered->front(), config.enable_nats,
                           config.nats_url);
}

void CvMmapClient::SetFrameCallback(OnFrameCallback &&cb) {
  pimpl_->on_frame_callback = std::move(cb);
}

void CvMmapClient::SetFramePlanesCallback(OnFramePlanesCallback &&cb) {
  pimpl_->on_frame_planes_callback = std::move(cb);
}

void CvMmapClient::SetBodyTrackingCallback(OnBodyTrackingCallback &&cb) {
  pimpl_->on_body_tracking_callback = std::move(cb);
  pimpl_->sync_nats_callbacks();
}

void CvMmapClient::SetEventCallback(OnEventCallback &&cb) {
  pimpl_->on_event_callback = std::move(cb);
  pimpl_->sync_nats_callbacks();
}

const std::string &CvMmapClient::Name() const { return pimpl_->instance_name; }

void CvMmapClient::Start() { return pimpl_->start(); }

void CvMmapClient::Stop() { return pimpl_->stop(); }

int32_t CvMmapClient::ResetFrameCount(std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return CONTROL_RESPONSE_UNSUPPORTED;
  }
  auto response = pimpl_->nats_client->ResetFrameCount(timeout);
  if (!response) {
    return response.error();
  }
  return *response;
}

cvmmap::expected<SourceInfo, int32_t>
CvMmapClient::GetSourceInfo(std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(CONTROL_RESPONSE_UNSUPPORTED);
  }
  return pimpl_->nats_client->GetSourceInfo(timeout);
}

cvmmap::expected<SeekResult, int32_t>
CvMmapClient::SeekTimestampNs(uint64_t timestamp_ns,
                              std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(CONTROL_RESPONSE_UNSUPPORTED);
  }
  return pimpl_->nats_client->SeekTimestampNs(timestamp_ns, timeout);
}

cvmmap::expected<PlaylistInfo, ControlError>
CvMmapClient::ApplyPlaylist(const PlaylistRequest &request,
                            std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(make_nats_disabled_error());
  }
  return pimpl_->nats_client->ApplyPlaylist(request, timeout);
}

cvmmap::expected<PlaylistInfo, ControlError>
CvMmapClient::GetPlaylistInfo(std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(make_nats_disabled_error());
  }
  return pimpl_->nats_client->GetPlaylistInfo(timeout);
}

cvmmap::expected<ControlCapabilities, ControlError>
CvMmapClient::GetCapabilities(std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(make_nats_disabled_error());
  }
  return pimpl_->nats_client->GetCapabilities(timeout);
}

cvmmap::expected<RecordingStatus, ControlError>
CvMmapClient::StartRecording(const RecordingRequest &request,
                             std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(make_nats_disabled_error());
  }
  return pimpl_->nats_client->StartRecording(request, timeout);
}

cvmmap::expected<RecordingStatus, ControlError>
CvMmapClient::StartRecording(std::string_view output_path,
                             std::chrono::milliseconds timeout) {
  return StartRecording(
      RecordingRequest{
          .format = RecordingFormat::Svo,
          .output_path = std::string(output_path),
      },
      timeout);
}

cvmmap::expected<RecordingStatus, ControlError>
CvMmapClient::StopRecording(RecordingFormat format,
                            std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(make_nats_disabled_error());
  }
  return pimpl_->nats_client->StopRecording(format, timeout);
}

cvmmap::expected<RecordingStatus, ControlError>
CvMmapClient::StopRecording(std::chrono::milliseconds timeout) {
  return StopRecording(RecordingFormat::Svo, timeout);
}

cvmmap::expected<RecordingStatus, ControlError>
CvMmapClient::GetRecordingStatus(RecordingFormat format,
                                 std::chrono::milliseconds timeout) {
  if (!pimpl_->nats_client) {
    return cvmmap::unexpected(make_nats_disabled_error());
  }
  return pimpl_->nats_client->GetRecordingStatus(format, timeout);
}

cvmmap::expected<RecordingStatus, ControlError>
CvMmapClient::GetRecordingStatus(std::chrono::milliseconds timeout) {
  return GetRecordingStatus(RecordingFormat::Svo, timeout);
}
} // namespace cvmmap
