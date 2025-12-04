#include <arpa/inet.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
  g_running = false;
}

std::string load_pipeline_description(const std::string &path) {
  std::ifstream ifs(path);
  if (!ifs) {
    throw std::runtime_error("Failed to open pipeline config: " + path);
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

std::string apply_camera_device(std::string pipeline_desc) {
  const char *env = std::getenv("APP_CAMERA_DEVICE");
  const std::string device =
      (env && *env) ? std::string(env) : std::string("/dev/video0");
  if (::access(device.c_str(), R_OK | W_OK) != 0) {
    std::cerr << "Specified camera device " << device
              << " not accessible: " << std::strerror(errno) << std::endl;
  }
  const std::string needle = "device=";
  const size_t pos = pipeline_desc.find(needle);
  if (pos != std::string::npos) {
    size_t start = pos + needle.size();
    size_t end = pipeline_desc.find_first_of(" !\t\r\n", start);
    const size_t length =
        (end == std::string::npos) ? std::string::npos : end - start;
    pipeline_desc.replace(start, length, device);
  }
  std::cout << "Using camera device: " << device << std::endl;
  return pipeline_desc;
}

uint16_t resolve_port(const char *env) {
  if (!env || *env == '\0') {
    return 8080;
  }
  int v = std::atoi(env);
  if (v <= 0 || v > 65535) {
    throw std::runtime_error("APP_HTTP_PORT is invalid");
  }
  return static_cast<uint16_t>(v);
}

class FrameStore {
 public:
  void update(const guint8 *data, gsize size) {
    if (!data || size == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    frame_.assign(data, data + size);
    ++sequence_;
    cond_.notify_all();
  }

  bool wait_for_frame(uint64_t &cursor, std::vector<uint8_t> &out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [&] { return sequence_ != cursor || !g_running.load(); });
    if (!g_running.load() && sequence_ == cursor) {
      return false;
    }
    out = frame_;
    cursor = sequence_;
    return !frame_.empty();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  std::vector<uint8_t> frame_;
  uint64_t sequence_{0};
};

bool send_all(int fd, const void *data, size_t len) {
  const auto *ptr = static_cast<const uint8_t *>(data);
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t written = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

void serve_client(int client_fd, FrameStore &store) {
  static const char kHeader[] =
      "HTTP/1.1 200 OK\r\n"
      "Cache-Control: no-cache\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

  if (!send_all(client_fd, kHeader, sizeof(kHeader) - 1)) {
    ::close(client_fd);
    return;
  }

  uint64_t cursor = 0;
  std::vector<uint8_t> frame;
  while (g_running.load()) {
    if (!store.wait_for_frame(cursor, frame)) {
      continue;
    }
    std::ostringstream oss;
    oss << "--frame\r\n"
        << "Content-Type: image/jpeg\r\n"
        << "Content-Length: " << frame.size() << "\r\n\r\n";
    const std::string prefix = oss.str();
    if (!send_all(client_fd, prefix.data(), prefix.size())) {
      break;
    }
    if (!frame.empty() && !send_all(client_fd, frame.data(), frame.size())) {
      break;
    }
    static const char kSuffix[] = "\r\n";
    if (!send_all(client_fd, kSuffix, sizeof(kSuffix) - 1)) {
      break;
    }
  }

  ::close(client_fd);
  std::cout << "[http] client disconnected" << std::endl;
}

int create_server_socket(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("socket() failed");
  }
  int enable = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    throw std::runtime_error("bind() failed");
  }
  if (::listen(fd, 4) < 0) {
    ::close(fd);
    throw std::runtime_error("listen() failed");
  }
  return fd;
}

}  // namespace

int main() {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  gst_init(nullptr, nullptr);

  const char *pipeline_env = std::getenv("PIPELINE_CONFIG");
  const std::string pipeline_path =
      pipeline_env && *pipeline_env ? pipeline_env
                                    : "configs/camera_preview.pipeline";
  
  std::cout << "Loading pipeline from: " << pipeline_path << std::endl;

  std::string pipeline_desc;
  try {
    pipeline_desc = load_pipeline_description(pipeline_path);
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
  pipeline_desc = apply_camera_device(pipeline_desc);

  GError *error = nullptr;
  GstElement *pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
  if (!pipeline) {
    std::cerr << "Failed to launch pipeline: "
              << (error ? error->message : "unknown error") << std::endl;
    if (error) {
      g_error_free(error);
    }
    return 1;
  }
  if (error) {
    g_error_free(error);
  }

  GstElement *appsink_elem =
      gst_bin_get_by_name(GST_BIN(pipeline), "preview_sink");
  if (!appsink_elem) {
    std::cerr << "appsink named 'preview_sink' not found in pipeline"
              << std::endl;
    gst_object_unref(pipeline);
    return 1;
  }
  GstAppSink *appsink = GST_APP_SINK(appsink_elem);
  gst_app_sink_set_max_buffers(appsink, 1);
  gst_app_sink_set_drop(appsink, TRUE);

  FrameStore frame_store;
  std::thread sample_thread([appsink, &frame_store]() {
    while (g_running.load()) {
      GstSample *sample =
          gst_app_sink_try_pull_sample(appsink, GST_SECOND / 2);
      if (!sample) {
        continue;
      }
      GstBuffer *buffer = gst_sample_get_buffer(sample);
      if (buffer) {
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
          frame_store.update(map.data, map.size);
          gst_buffer_unmap(buffer, &map);
        }
      }
      gst_sample_unref(sample);
    }
  });

  GstBus *bus = gst_element_get_bus(pipeline);
  std::thread bus_thread([bus]() {
    while (g_running.load()) {
      GstMessage *msg =
          gst_bus_timed_pop_filtered(bus, GST_SECOND / 5,
                                     static_cast<GstMessageType>(
                                         GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
      if (!msg) {
        continue;
      }
      switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
          GError *err = nullptr;
          gchar *dbg = nullptr;
          gst_message_parse_error(msg, &err, &dbg);
          std::cerr << "[gstreamer] ERROR: "
                    << (err ? err->message : "unknown") << std::endl;
          if (dbg) {
            std::cerr << "  debug: " << dbg << std::endl;
          }
          if (err) {
            g_error_free(err);
          }
          g_free(dbg);
          g_running = false;
          break;
        }
        case GST_MESSAGE_EOS:
          std::cerr << "[gstreamer] End of stream" << std::endl;
          g_running = false;
          break;
        default:
          break;
      }
      gst_message_unref(msg);
    }
  });

  int server_fd = -1;
  try {
    const uint16_t port = resolve_port(std::getenv("APP_HTTP_PORT"));
    server_fd = create_server_socket(port);
    std::cout << "HTTP MJPEG preview available at port " << port << std::endl;
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << std::endl;
  }

  std::thread accept_thread;
  if (server_fd >= 0) {
    accept_thread = std::thread([server_fd, &frame_store]() {
      while (g_running.load()) {
        sockaddr_in addr {};
        socklen_t len = sizeof(addr);
        int client = ::accept(server_fd, reinterpret_cast<sockaddr *>(&addr),
                              &len);
        if (client < 0) {
          if (errno == EINTR) {
            continue;
          }
          std::cerr << "accept() failed: " << std::strerror(errno) << std::endl;
          break;
        }
        std::cout << "[http] client connected" << std::endl;
        std::thread(serve_client, client, std::ref(frame_store)).detach();
      }
    });
  }

  GstStateChangeReturn state_ret =
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (state_ret == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to start pipeline" << std::endl;
    g_running = false;
  }

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);

  if (server_fd >= 0) {
    ::shutdown(server_fd, SHUT_RDWR);
    ::close(server_fd);
  }

  if (accept_thread.joinable()) {
    accept_thread.join();
  }
  if (bus_thread.joinable()) {
    bus_thread.join();
  }
  if (sample_thread.joinable()) {
    sample_thread.join();
  }

  gst_object_unref(appsink_elem);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return 0;
}
