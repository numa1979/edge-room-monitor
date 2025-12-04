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

// DeepStream headers
#include "gstnvdsmeta.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
  g_running = false;
}

struct Detection {
  uint64_t tracking_id;
  int class_id;
  float confidence;
  float left;
  float top;
  float width;
  float height;
};

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

class DetectionStore {
 public:
  void update(const std::vector<Detection> &detections) {
    std::lock_guard<std::mutex> lock(mutex_);
    detections_ = detections;
  }

  std::vector<Detection> get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return detections_;
  }

  void set_selected(uint64_t tracking_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    selected_tracking_id_ = tracking_id;
    has_selection_ = true;
  }

  void clear_selected() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_selection_ = false;
    selected_tracking_id_ = 0;
  }

  bool get_selected(uint64_t &out_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_selection_) {
      out_id = selected_tracking_id_;
      return true;
    }
    return false;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Detection> detections_;
  uint64_t selected_tracking_id_{0};
  bool has_selection_{false};
};

std::string detections_to_json(const std::vector<Detection> &detections) {
  std::ostringstream oss;
  oss << "{\"detections\":[";
  for (size_t i = 0; i < detections.size(); ++i) {
    if (i > 0) oss << ",";
    const auto &d = detections[i];
    oss << "{"
        << "\"tracking_id\":" << d.tracking_id << ","
        << "\"class_id\":" << d.class_id << ","
        << "\"confidence\":" << d.confidence << ","
        << "\"bbox\":{\"left\":" << d.left << ",\"top\":" << d.top
        << ",\"width\":" << d.width << ",\"height\":" << d.height << "}"
        << "}";
  }
  oss << "]}";
  return oss.str();
}

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

void serve_mjpeg_client(int client_fd, FrameStore &store) {
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
  std::cout << "[http] MJPEG client disconnected" << std::endl;
}

std::string read_request_body(int client_fd, size_t content_length) {
  if (content_length == 0 || content_length > 4096) {
    return "";
  }
  std::vector<char> buffer(content_length);
  size_t total_read = 0;
  while (total_read < content_length) {
    ssize_t n = ::recv(client_fd, buffer.data() + total_read, 
                       content_length - total_read, 0);
    if (n <= 0) break;
    total_read += n;
  }
  return std::string(buffer.data(), total_read);
}

uint64_t parse_tracking_id_from_json(const std::string &json) {
  // Simple JSON parsing: {"tracking_id":123}
  size_t pos = json.find("\"tracking_id\"");
  if (pos == std::string::npos) return 0;
  pos = json.find(":", pos);
  if (pos == std::string::npos) return 0;
  pos++;
  while (pos < json.size() && std::isspace(json[pos])) pos++;
  uint64_t id = 0;
  while (pos < json.size() && std::isdigit(json[pos])) {
    id = id * 10 + (json[pos] - '0');
    pos++;
  }
  return id;
}

void serve_api_client(int client_fd, const std::string &request,
                      DetectionStore &detection_store) {
  std::string response_body;
  std::string status = "200 OK";
  
  // Parse request method and path
  size_t method_end = request.find(' ');
  if (method_end == std::string::npos) {
    response_body = "{\"error\":\"Bad request\"}";
    status = "400 Bad Request";
  } else {
    std::string method = request.substr(0, method_end);
    size_t path_start = method_end + 1;
    size_t path_end = request.find(' ', path_start);
    std::string path = request.substr(path_start, path_end - path_start);
    
    if (method == "GET" && path == "/api/detections") {
      auto detections = detection_store.get();
      response_body = detections_to_json(detections);
    } else if (method == "POST" && path == "/api/select") {
      // Parse Content-Length
      size_t cl_pos = request.find("Content-Length:");
      size_t content_length = 0;
      if (cl_pos != std::string::npos) {
        content_length = std::atoi(request.c_str() + cl_pos + 15);
      }
      
      // Read body
      size_t body_start = request.find("\r\n\r\n");
      std::string body;
      if (body_start != std::string::npos) {
        body = request.substr(body_start + 4);
        if (body.size() < content_length) {
          body += read_request_body(client_fd, content_length - body.size());
        }
      }
      
      uint64_t tracking_id = parse_tracking_id_from_json(body);
      if (tracking_id > 0) {
        detection_store.set_selected(tracking_id);
        response_body = "{\"status\":\"selected\",\"tracking_id\":" + 
                       std::to_string(tracking_id) + "}";
        std::cout << "[api] Selected tracking_id: " << tracking_id << std::endl;
      } else {
        response_body = "{\"error\":\"Invalid tracking_id\"}";
        status = "400 Bad Request";
      }
    } else if (method == "POST" && path == "/api/unselect") {
      detection_store.clear_selected();
      response_body = "{\"status\":\"unselected\"}";
      std::cout << "[api] Unselected" << std::endl;
    } else {
      response_body = "{\"error\":\"Not found\"}";
      status = "404 Not Found";
    }
  }

  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << response_body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n\r\n"
      << response_body;

  const std::string response = oss.str();
  send_all(client_fd, response.data(), response.size());
  ::close(client_fd);
}

std::string read_http_request(int client_fd) {
  char buffer[4096];
  ssize_t n = ::recv(client_fd, buffer, sizeof(buffer) - 1, 0);
  if (n <= 0) {
    return "";
  }
  buffer[n] = '\0';
  return std::string(buffer);
}

bool is_api_request(const std::string &request) {
  return request.find("GET /api/") == 0 || 
         request.find("POST /api/") == 0;
}

void serve_html_file(int client_fd, const std::string &filepath) {
  std::ifstream file(filepath);
  if (!file) {
    const char *response = 
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n\r\n"
        "File not found";
    send_all(client_fd, response, std::strlen(response));
    ::close(client_fd);
    return;
  }
  
  std::ostringstream content;
  content << file.rdbuf();
  std::string body = content.str();
  
  std::ostringstream response;
  response << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: text/html; charset=utf-8\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << body;
  
  std::string resp_str = response.str();
  send_all(client_fd, resp_str.data(), resp_str.size());
  ::close(client_fd);
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
  DetectionStore detection_store;
  
  std::thread sample_thread([appsink, &frame_store, &detection_store]() {
    while (g_running.load()) {
      GstSample *sample =
          gst_app_sink_try_pull_sample(appsink, GST_SECOND / 2);
      if (!sample) {
        continue;
      }
      GstBuffer *buffer = gst_sample_get_buffer(sample);
      if (buffer) {
        // Extract DeepStream metadata
        std::vector<Detection> detections;
        NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buffer);
        if (batch_meta) {
          for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame;
               l_frame = l_frame->next) {
            NvDsFrameMeta *frame_meta =
                static_cast<NvDsFrameMeta *>(l_frame->data);
            for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj;
                 l_obj = l_obj->next) {
              NvDsObjectMeta *obj_meta =
                  static_cast<NvDsObjectMeta *>(l_obj->data);
              Detection d;
              d.tracking_id = obj_meta->object_id;
              d.class_id = obj_meta->class_id;
              d.confidence = obj_meta->confidence;
              d.left = obj_meta->rect_params.left;
              d.top = obj_meta->rect_params.top;
              d.width = obj_meta->rect_params.width;
              d.height = obj_meta->rect_params.height;
              detections.push_back(d);
            }
          }
        }
        detection_store.update(detections);
        
        // Extract JPEG frame
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
    std::cout << "HTTP server available at port " << port << std::endl;
    std::cout << "  - MJPEG stream: http://[ip]:" << port << "/" << std::endl;
    std::cout << "  - Detections API: http://[ip]:" << port << "/api/detections" << std::endl;
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << std::endl;
  }

  std::thread accept_thread;
  if (server_fd >= 0) {
    accept_thread = std::thread([server_fd, &frame_store, &detection_store]() {
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
        
        // Read HTTP request
        std::string request = read_http_request(client);
        if (request.empty()) {
          ::close(client);
          continue;
        }
        
        std::cout << "[http] " << request.substr(0, request.find('\r')) << std::endl;
        
        if (is_api_request(request)) {
          std::thread(serve_api_client, client, request,
                      std::ref(detection_store)).detach();
        } else if (request.find("GET /stream") == 0) {
          std::thread(serve_mjpeg_client, client, std::ref(frame_store)).detach();
        } else if (request.find("GET /debug") == 0) {
          std::thread(serve_html_file, client, 
                      std::string("/workspace/edge-room-monitor/ui/debug.html")).detach();
        } else {
          // Default: serve HTML UI
          std::thread(serve_html_file, client, 
                      std::string("/workspace/edge-room-monitor/ui/mjpeg_viewer.html")).detach();
        }
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
