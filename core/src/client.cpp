#include <cvmmap/client.hpp>
#include <cvmmap/parser.hpp>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <format>
#include <memory>
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

struct SharedBuffer {
  SharedBuffer(int shm_fd) {
    shm_fd_ = shm_fd;
    if (shm_fd_ < 0) {
      throw std::invalid_argument("invalid shared memory file descriptor");
    }

    struct stat sb;
    if (fstat(shm_fd_, &sb) < 0) {
      throw std::runtime_error(
          std::format("fstat {}({})", errno, strerror(errno)));
    }
    shm_size_ = sb.st_size;

    shm_ptr_ = static_cast<uint8_t *>(
        mmap(nullptr, shm_size_, PROT_READ, MAP_SHARED, shm_fd_, 0));
    if (shm_ptr_ == MAP_FAILED) {
      throw std::runtime_error(
          std::format("mmap {}({})", errno, strerror(errno)));
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

  std::expected<std::monostate, std::string> verify() {
    if (shm_size_ < SHM_PAYLOAD_OFFSET) {
      return std::unexpected(std::format("shared memory too small: {} < {}",
                                         shm_size_, SHM_PAYLOAD_OFFSET));
    }

    const auto &metadata =
        *reinterpret_cast<const frame_metadata_t *>(metadata_data_);
    if (std::equal(metadata.magic, metadata.magic + CV_MMAP_MAGIC_LEN,
                   frame_metadata_t::CV_MMAP_MAGIC.data()) == false) {
      return std::unexpected("invalid magic");
    }

    if (metadata.versions_major == 0 && metadata.versions_minor == 0) {
      return {std::monostate{}};
    }

    if (metadata.versions_major != FRAME_METADATA_V1_MAJOR &&
        metadata.versions_major != FRAME_METADATA_V2_MAJOR) {
      return std::unexpected(std::format(
          "incompatible major version; got {}.{}, expected {}.x or {}.x",
          metadata.versions_major, metadata.versions_minor,
          FRAME_METADATA_V1_MAJOR, FRAME_METADATA_V2_MAJOR));
    }

    auto parsed = parse_frame_metadata_regions(
        std::span<const uint8_t>(metadata_data_, SHM_PAYLOAD_OFFSET),
        std::span<const uint8_t>(image_data_, shm_size_ - SHM_PAYLOAD_OFFSET));
    if (!parsed) {
      return std::unexpected(parsed.error());
    }

    normalized_metadata_ = parsed->normalized_metadata;
    left_plane_ = parsed->left_plane;
    depth_info_ = parsed->depth_info;
    depth_plane_ = parsed->depth_plane;
    confidence_info_ = parsed->confidence_info;
    confidence_plane_ = parsed->confidence_plane;
    return {std::monostate{}};
  }

  std::span<const uint8_t> image_data() const { return left_plane_; }

  const frame_metadata_t &metadata() const { return normalized_metadata_; }

  frame_planes_view_t planes() const {
    return frame_planes_view_t{
        .left = left_plane_,
        .depth_info = depth_info_,
        .depth = depth_plane_,
        .confidence_info = confidence_info_,
        .confidence = confidence_plane_,
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
  std::optional<frame_info_t> depth_info_{};
  std::span<const uint8_t> depth_plane_{};
  std::optional<frame_info_t> confidence_info_{};
  std::span<const uint8_t> confidence_plane_{};
};

struct CvMmapClient::impl {
  /**
   * inputs, expected to be filled
   */

  std::string instance_name{};
  std::string shm_name{};
  std::string zmq_addr{};
  std::string zmq_control_addr{};

  bool has_init = false;
  std::atomic_bool is_running = false;

  zmq::socket_t socket{};
  zmq::socket_t control_socket{};
  std::mutex control_mutex{}; // protect control_socket for thread-safe access

  std::unique_ptr<SharedBuffer> shared_buffer{};
  CvMmapClient::OnFrameCallback on_frame_callback{};
  CvMmapClient::OnFramePlanesCallback on_frame_planes_callback{};
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

  void polling_task_();

  /**
   * @brief Send a control message and wait for response
   * @param command_id The command to send
   * @param timeout Timeout for the request
   * @param payload Optional request payload data
   * @return Response code from server, or CONTROL_RESPONSE_TIMEOUT on timeout
   */
  int32_t send_control_request(int32_t command_id,
                               std::chrono::milliseconds timeout,
                               std::span<const uint8_t> payload = {});
};

void CvMmapClient::impl::init() {
  if (has_init) {
    return;
  }
  socket = zmq::socket_t(app::global_zmq_context(), zmq::socket_type::sub);
  control_socket = zmq::socket_t(app::global_zmq_context(), zmq::socket_type::req);
  has_init = true;
}

void CvMmapClient::impl::start() {
  if (is_running.load(std::memory_order_acquire)) {
    return;
  }
  socket.set(zmq::sockopt::conflate_t{}, true);
  socket.set(zmq::sockopt::rcvtimeo_t{}, 100); // 100ms timeout
  socket.connect(zmq_addr);
  socket.set(zmq::sockopt::subscribe_t{}, "");

  // Connect control socket (lazy connect, actual timeout set per-request)
  control_socket.connect(zmq_control_addr);

  is_running.store(true, std::memory_order_release);
  polling_thread =
      std::make_unique<std::thread>(&CvMmapClient::impl::polling_task_, this);
  if (not polling_thread) {
    is_running.store(false, std::memory_order_release);
    throw std::runtime_error("failed to create thread");
  }
}

void CvMmapClient::impl::stop() {
  if (not has_init || not is_running.load(std::memory_order_acquire)) {
    return;
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
    auto message = zmq::message_t{};
    auto res = socket.recv(message, zmq::recv_flags::none);
    if (not res) {
      continue;
    }
    const auto buf =
        std::span<const uint8_t>(static_cast<uint8_t *>(message.data()), *res);

    if (buf.empty()) {
      continue;
    }

    const uint8_t magic = buf[0];

    // Handle module status messages
    if (magic == MODULE_STATUS_MAGIC) {
      if (buf.size() < sizeof(module_status_message_t)) {
        spdlog::error("unexpected `module_status_message` size {}", buf.size());
        continue;
      }
      const auto &status_msg =
          *reinterpret_cast<const module_status_message_t *>(buf.data());
      std::string_view label = status_msg.label();
      if (label != instance_name) {
        spdlog::debug("status label mismatch: {} != {}", label, instance_name);
        continue;
      }
      if (on_event_callback) {
        on_event_callback(static_cast<ModuleStatus>(status_msg.module_status));
      }
      continue;
    }

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

int32_t
CvMmapClient::impl::send_control_request(int32_t command_id,
                                         std::chrono::milliseconds timeout_ms,
                                         std::span<const uint8_t> payload) {
  std::lock_guard<std::mutex> lock(control_mutex);

  // Set timeout for this request
  control_socket.set(zmq::sockopt::rcvtimeo,
                     static_cast<int>(timeout_ms.count()));
  control_socket.set(zmq::sockopt::sndtimeo,
                     static_cast<int>(timeout_ms.count()));

  // Small object optimization: use stack buffer for small payloads, heap for
  // large ones
  constexpr size_t SSO_THRESHOLD = 64;
  const size_t total_size = sizeof(control_message_request_t) + payload.size();

  alignas(control_message_request_t)
      uint8_t stack_buffer[sizeof(control_message_request_t) + SSO_THRESHOLD];
  std::unique_ptr<uint8_t[]> heap_buffer;
  uint8_t *buffer_ptr;

  if (payload.size() <= SSO_THRESHOLD) {
    buffer_ptr = stack_buffer;
  } else {
    heap_buffer = std::make_unique<uint8_t[]>(total_size);
    buffer_ptr = heap_buffer.get();
  }

  // Build request message using placement new
  auto *request = new (buffer_ptr) control_message_request_t{};
  request->command_id = command_id;
  request->set_label(instance_name);
  request->request_message_length = static_cast<uint16_t>(payload.size());
  if (!payload.empty()) {
    std::copy(payload.begin(), payload.end(),
              buffer_ptr + sizeof(control_message_request_t));
  }

  // Send request
  try {
    auto send_res = control_socket.send(zmq::buffer(buffer_ptr, total_size),
                                        zmq::send_flags::none);
    if (!send_res) {
      spdlog::error("control request send timeout");
      return CONTROL_RESPONSE_TIMEOUT;
    }
  } catch (const zmq::error_t &e) {
    spdlog::error("control request send error: {}", e.what());
    return CONTROL_RESPONSE_ERROR;
  }

  // Receive response
  zmq::message_t response_msg;
  try {
    auto recv_res = control_socket.recv(response_msg, zmq::recv_flags::none);
    if (!recv_res) {
      spdlog::error("control response recv timeout");
      return CONTROL_RESPONSE_TIMEOUT;
    }
  } catch (const zmq::error_t &e) {
    spdlog::error("control response recv error: {}", e.what());
    return CONTROL_RESPONSE_ERROR;
  }

  if (response_msg.size() < sizeof(control_message_response_t)) {
    spdlog::error("control response too small: {} bytes", response_msg.size());
    return CONTROL_RESPONSE_INVALID_MSG_SIZE;
  }

  const auto *response =
      static_cast<const control_message_response_t *>(response_msg.data());
  if (response->_magic != CONTROL_MESSAGE_RESPONSE_MAGIC) {
    spdlog::error("control response invalid magic: 0x{:02x}", response->_magic);
    return CONTROL_RESPONSE_INVALID_MAGIC;
  }

  return response->response_code;
}

CvMmapClient::CvMmapClient(const std::string &instance_name)
    : pimpl_(std::make_unique<impl>()) {
  auto resolved = resolve_cvmmap_target_or_throw(instance_name);
  pimpl_->instance_name = resolved.instance;
  pimpl_->shm_name = resolved.shm_name;
  pimpl_->zmq_addr = resolved.zmq_addr;
  pimpl_->zmq_control_addr = resolved.zmq_control_addr;
  pimpl_->init();
}

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

void CvMmapClient::SetFrameCallback(OnFrameCallback &&cb) {
  pimpl_->on_frame_callback = std::move(cb);
}

void CvMmapClient::SetFramePlanesCallback(OnFramePlanesCallback &&cb) {
  pimpl_->on_frame_planes_callback = std::move(cb);
}

void CvMmapClient::SetEventCallback(OnEventCallback &&cb) {
  pimpl_->on_event_callback = std::move(cb);
}

const std::string &CvMmapClient::Name() const { return pimpl_->instance_name; }

void CvMmapClient::Start() { return pimpl_->start(); }

void CvMmapClient::Stop() { return pimpl_->stop(); }

int32_t CvMmapClient::ResetFrameCount(std::chrono::milliseconds timeout) {
  return pimpl_->send_control_request(CONTROL_MSG_CMD_RESET_FRAME_COUNT,
                                      timeout);
}
} // namespace cvmmap
