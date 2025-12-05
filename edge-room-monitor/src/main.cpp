#include <arpa/inet.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
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
  uint64_t tracking_id;  // nvtrackerの一時ID
  int class_id;
  float confidence;
  float left;
  float top;
  float width;
  float height;
};

enum AlertType {
  ALERT_NONE = 0,
  ALERT_FALL = 1,           // 転倒
  ALERT_BED_FALL = 2,       // ベッドから落下
  ALERT_BED_EXIT = 3,       // ベッド離脱
  ALERT_LYING_FLOOR = 4,    // 床で横たわり
  ALERT_FRAME_OUT = 5       // フレームアウト（徘徊の可能性）
};

struct Alert {
  int fixed_id;
  AlertType type;
  std::chrono::steady_clock::time_point timestamp;
  std::string message;
  bool acknowledged;  // 確認済みフラグ
};

struct RegisteredPerson {
  int fixed_id;  // 固定ID (0-3)
  uint64_t current_nvtracker_id;  // 現在のnvtracker ID
  float bbox_width;
  float bbox_height;
  float bbox_left;
  float bbox_top;
  float stable_bbox_top;  // 安定時の頭の位置（Y座標）
  float stable_bbox_height;  // 安定時の高さ（立っている時）
  float sitting_bbox_height;  // 座っている時の高さ
  float prev_bbox_top;  // 前フレームの頭位置（転倒検知用）
  float prev_bbox_height;  // 前フレームの高さ（転倒検知用）
  float lying_bbox_top;  // 横たわり開始時のY座標（ベッド落下検知用）
  std::chrono::steady_clock::time_point last_seen;
  std::chrono::steady_clock::time_point lying_start;
  std::chrono::steady_clock::time_point lying_stable;  // 横たわり状態が安定した時刻
  std::chrono::steady_clock::time_point standing_confirmed;  // 立っている状態が確定した時刻
  std::chrono::steady_clock::time_point sitting_confirmed;  // 座っている状態が確定した時刻
  std::chrono::steady_clock::time_point head_position_recorded;  // 頭の位置を記録した時刻
  std::chrono::steady_clock::time_point last_update;  // 最後に更新した時刻（転倒検知用）
  int frame_count;  // フレームカウント
  bool active;  // 追跡中かどうか
  bool is_lying;  // 横たわっているか
  bool is_sitting;  // 座っているか
  bool was_standing;  // 前は立っていたか（確定状態）
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
  static constexpr int MAX_REGISTERED_PERSONS = 4;  // 最大4人（Jetson Nano性能考慮）
  
 private:
  bool auto_register_enabled_ = true;  // 自動登録モード
  
 public:
  void set_auto_register(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto_register_enabled_ = enabled;
    std::cout << "[config] Auto-register mode: " << (enabled ? "enabled" : "disabled") << std::endl;
  }
  
  bool get_auto_register() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return auto_register_enabled_;
  }
  
  DetectionStore() {
    // 配列を初期化（全員未登録状態）
    for (auto &person : registered_persons_) {
      person.fixed_id = -1;
      person.current_nvtracker_id = 0;
      person.bbox_width = 0.0f;
      person.bbox_height = 0.0f;
      person.bbox_left = 0.0f;
      person.bbox_top = 0.0f;
      person.stable_bbox_top = 0.0f;
      person.stable_bbox_height = 0.0f;
      person.sitting_bbox_height = 0.0f;
      person.prev_bbox_top = 0.0f;
      person.prev_bbox_height = 0.0f;
      person.lying_bbox_top = 0.0f;
      person.frame_count = 0;
      person.active = false;
      person.is_lying = false;
      person.is_sitting = false;
      person.was_standing = false;
    }
  }
  

  
  std::vector<Alert> get_alerts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alerts_;
  }
  
  void acknowledge_alert(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < alerts_.size()) {
      alerts_[index].acknowledged = true;
    }
  }
  
  void acknowledge_alerts_for_person(int fixed_id) {
    // 注意: この関数は既にmutex_がロックされている状態で呼ばれる
    // 内部用の関数なのでロックしない
    for (auto &alert : alerts_) {
      if (alert.fixed_id == fixed_id && !alert.acknowledged) {
        alert.acknowledged = true;
        std::cout << "[Alert] Auto-acknowledged alert for ID " << fixed_id << std::endl;
      }
    }
  }
  
  void clear_alerts() {
    std::lock_guard<std::mutex> lock(mutex_);
    alerts_.clear();
  }
  

  
  void update(const std::vector<Detection> &detections) {
    std::lock_guard<std::mutex> lock(mutex_);
    detections_ = detections;
    auto now = std::chrono::steady_clock::now();
    
    // 自動登録: 未登録の検出を自動で追跡開始（モードが有効な場合のみ）
    if (auto_register_enabled_) {
      for (const auto &det : detections) {
        bool already_tracked = false;
        for (const auto &person : registered_persons_) {
          if (person.active && person.current_nvtracker_id == det.tracking_id) {
            already_tracked = true;
            break;
          }
        }
        
        if (!already_tracked) {
          // 空きスロットを探して自動登録（最大4人まで）
          for (auto &person : registered_persons_) {
            if (!person.active) {
              person.fixed_id = &person - &registered_persons_[0];
              person.current_nvtracker_id = det.tracking_id;
            person.bbox_width = det.width;
            person.bbox_height = det.height;
            person.bbox_left = det.left;
            person.bbox_top = det.top;
            person.stable_bbox_top = det.top;  // 初期頭位置を記録
            person.stable_bbox_height = det.height;  // 初期高さを記録
            person.sitting_bbox_height = 0.0f;
            person.prev_bbox_top = det.top;
            person.prev_bbox_height = det.height;
            person.lying_bbox_top = 0.0f;
            person.last_seen = now;
            person.last_update = now;
            person.lying_start = now;
            person.lying_stable = now;
            person.standing_confirmed = now;
            person.sitting_confirmed = now;
            person.head_position_recorded = now;
            person.frame_count = 0;
            person.active = true;
            person.is_lying = (det.width > det.height * 1.8f);  // 1.8倍で横たわり判定
            person.is_sitting = false;
            person.was_standing = !person.is_lying;
            
            std::cout << "[Auto] Registered nvtracker=" << det.tracking_id 
                     << " as Fixed ID " << person.fixed_id << std::endl;
            break;
          }
        }
      }
    }
    
    // 登録済み人物の追跡と異常検知
    for (auto &person : registered_persons_) {
      if (!person.active) continue;
      
      bool found = false;
      for (const auto &det : detections) {
        if (det.tracking_id == person.current_nvtracker_id) {
          found = true;
          
          // フレームカウント
          person.frame_count++;
          
          // 転倒検知: 前フレームとの比較（10フレーム以上追跡後）
          if (person.frame_count >= 10 && person.was_standing && person.prev_bbox_height > 100.0f) {
            auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - person.last_update).count();
            
            // 急激な変化を検知（2秒以内）
            if (time_diff > 0 && time_diff <= 2000) {
              // 高さが50%以上減少（立っている→しゃがむ/倒れる）
              float height_ratio = det.height / person.prev_bbox_height;
              // 頭の位置が大きく下がった（画面下方向 = Y座標増加）
              float top_diff = det.top - person.prev_bbox_top;
              
              // デバッグ: 急激な変化を検出
              if (height_ratio < 0.7f && top_diff > 50.0f) {
                std::cout << "[Fall Check] ID " << person.fixed_id 
                         << " height_ratio:" << std::fixed << std::setprecision(2) << height_ratio
                         << " top_diff:" << (int)top_diff 
                         << " prev_h:" << (int)person.prev_bbox_height << std::endl;
              }
              
              // 転倒条件: 高さが30%以上減少 OR 頭が大きく下がった
              if ((height_ratio < 0.7f && top_diff > person.prev_bbox_height * 0.3f) ||
                  (height_ratio < 0.5f && top_diff > person.prev_bbox_height * 0.15f)) {
                // 転倒検知！（add_alert内で重複チェックあり）
                add_alert(person.fixed_id, ALERT_FALL, 
                         "Sudden fall detected", now);
                // ログは1回だけ出す（重複防止）
                bool already_alerted = false;
                for (const auto &alert : alerts_) {
                  if (alert.fixed_id == person.fixed_id && alert.type == ALERT_FALL && !alert.acknowledged) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - alert.timestamp).count();
                    if (elapsed < 5) {
                      already_alerted = true;
                      break;
                    }
                  }
                }
                if (!already_alerted) {
                  std::cout << "[Alert] Fixed ID " << person.fixed_id 
                           << " FALL detected! height:" << (int)person.prev_bbox_height 
                           << "->" << (int)det.height 
                           << " top:" << (int)person.prev_bbox_top << "->" << (int)det.top << std::endl;
                }
              }
            }
          }
          
          // 位置・姿勢情報を更新
          person.prev_bbox_top = person.bbox_top;
          person.prev_bbox_height = person.bbox_height;
          person.bbox_width = det.width;
          person.bbox_height = det.height;
          person.bbox_left = det.left;
          person.bbox_top = det.top;
          person.last_seen = now;
          person.last_update = now;
          
          // 姿勢判定
          // 横たわり = 幅が高さより大きい（横向きbbox）
          bool is_lying = (det.width > det.height * 1.2f);  // 1.2倍で横たわり判定（より敏感に）
          bool is_sitting = false;
          
          // デバッグ: 姿勢判定（15フレームごと = 約1秒）
          if (person.frame_count % 15 == 0) {
            float ratio = det.width / det.height;
            std::cout << "[Debug] ID " << person.fixed_id 
                     << " bbox:" << (int)det.width << "x" << (int)det.height 
                     << " ratio:" << std::fixed << std::setprecision(2) << ratio 
                     << " lying:" << (is_lying ? "YES" : "NO") << std::endl;
          }
          
          // 座っている判定: 立っている時の60-80%の高さ
          if (!is_lying && person.stable_bbox_height > 100.0f) {
            float height_ratio = det.height / person.stable_bbox_height;
            is_sitting = (height_ratio >= 0.55f && height_ratio <= 0.85f);
          }
          
          // 立っている状態が3秒以上続いたら確定
          if (!is_lying && !is_sitting) {
            auto standing_sec = std::chrono::duration_cast<std::chrono::seconds>(
                now - person.standing_confirmed).count();
            if (standing_sec >= 3) {
              person.was_standing = true;
              // 安定時の高さと頭位置を更新（立っている時の平均）
              person.stable_bbox_height = (person.stable_bbox_height * 0.8f + det.height * 0.2f);
              person.stable_bbox_top = (person.stable_bbox_top * 0.8f + det.top * 0.2f);
              person.head_position_recorded = now;
            }
          } else {
            // 横たわったら立っている確定時刻をリセット
            person.standing_confirmed = now;
          }
          
          // 座っている状態が2秒以上続いたら確定
          if (is_sitting) {
            auto sitting_sec = std::chrono::duration_cast<std::chrono::seconds>(
                now - person.sitting_confirmed).count();
            if (sitting_sec >= 2) {
              person.is_sitting = true;
              // 座っている時の高さを記録
              person.sitting_bbox_height = (person.sitting_bbox_height * 0.7f + det.height * 0.3f);
            }
          } else {
            person.sitting_confirmed = now;
            if (!is_lying) {
              person.is_sitting = false;
            }
          }
          
          // 異常検知
          check_alerts(person, det, is_lying, now);
          
          person.is_lying = is_lying;
          
          break;
        }
      }
      
      // 見つからない場合（bbox消失）
      if (!found) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - person.last_seen).count();
        
        // 10秒以上見失ったら徘徊の可能性としてアラート
        if (elapsed >= 10 && elapsed < 11) {
          add_alert(person.fixed_id, ALERT_FRAME_OUT, 
                   "Left the frame - possible wandering", now);
          std::cout << "[Alert] Fixed ID " << person.fixed_id 
                   << " left the frame (>10s) - possible wandering" << std::endl;
        }
        
        // 60秒以上見失ったら追跡解除
        if (elapsed >= 60) {
          std::cout << "[Track] Fixed ID " << person.fixed_id 
                   << " tracking stopped (>60s)" << std::endl;
          person.active = false;
        }
      }
    }
  }  // end of tracking loop
}  // end of update()
  
 private:
  void check_alerts(RegisteredPerson &person, const Detection &det, bool is_lying,
                   std::chrono::steady_clock::time_point now) {
    
    // 最低10フレーム（約2秒）追跡してから異常検知開始
    if (person.frame_count < 10) {
      return;
    }
    
    // 横たわり状態の記録とベッド落下検知
    if (is_lying) {
      if (person.lying_start.time_since_epoch().count() == 0 || !person.is_lying) {
        person.lying_start = now;
        person.lying_stable = now;
        person.lying_bbox_top = det.top;
        std::cout << "[State] ID " << person.fixed_id 
                 << " 縦長→横長 (lying down at Y:" << (int)det.top << ")" << std::endl;
      } else {
        // 横たわり状態が3秒以上続いたら安定とみなす
        auto lying_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - person.lying_start).count();
        if (lying_sec >= 3) {
          auto stable_sec = std::chrono::duration_cast<std::chrono::seconds>(
              now - person.lying_stable).count();
          if (stable_sec == 0) {
            person.lying_stable = now;
            person.lying_bbox_top = det.top;
          }
          
          // ベッド落下検知: 横たわり状態から急激にY座標が増加（下に落ちた）
          float top_diff = det.top - person.lying_bbox_top;
          if (top_diff > 150.0f) {  // 150px以上下がったら落下
            add_alert(person.fixed_id, ALERT_BED_FALL, 
                     "Bed fall detected", now);
            std::cout << "[Alert] Fixed ID " << person.fixed_id 
                     << " BED FALL detected! Y:" << (int)person.lying_bbox_top 
                     << "->" << (int)det.top << " (diff:" << (int)top_diff << ")" << std::endl;
            // 落下後は新しい位置を基準に
            person.lying_bbox_top = det.top;
            person.lying_stable = now;
          }
        }
      }
    } else {
      if (person.lying_start.time_since_epoch().count() != 0) {
        auto lying_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - person.lying_start).count();
        std::cout << "[State] ID " << person.fixed_id 
                 << " 横長→縦長 (standing up, was lying for " 
                 << lying_sec << "s)" << std::endl;
        // 起き上がったら、転倒・落下アラートを自動確認
        acknowledge_alerts_for_person(person.fixed_id);
      }
      person.lying_start = std::chrono::steady_clock::time_point();
      person.lying_stable = std::chrono::steady_clock::time_point();
      person.lying_bbox_top = 0.0f;
    }
  }
  
  void add_alert(int fixed_id, AlertType type, const std::string &message,
                std::chrono::steady_clock::time_point timestamp) {
    // 重複アラート防止（同じ人の同じタイプのアラートが最近あれば追加しない）
    for (const auto &alert : alerts_) {
      if (alert.fixed_id == fixed_id && alert.type == type && !alert.acknowledged) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            timestamp - alert.timestamp).count();
        if (elapsed < 30) {  // 30秒以内は重複とみなす
          return;  // 重複
        }
      }
    }
    
    Alert alert;
    alert.fixed_id = fixed_id;
    alert.type = type;
    alert.message = message;
    alert.timestamp = timestamp;
    alert.acknowledged = false;
    alerts_.push_back(alert);
    
    std::cout << "[Alert] Fixed ID " << fixed_id << ": " << message << std::endl;
  }
  
 public:
  // 検出情報を取得（固定IDマッピング付き）
  struct DetectionWithFixedId {
    Detection detection;
    int fixed_id;  // -1 = 未登録
  };
  
  std::vector<DetectionWithFixedId> get_with_fixed_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DetectionWithFixedId> result;
    
    for (const auto &det : detections_) {
      DetectionWithFixedId dwf;
      dwf.detection = det;
      dwf.fixed_id = -1;  // デフォルトは未登録
      
      // 登録済み人物のnvtracker IDと一致するか確認
      for (const auto &person : registered_persons_) {
        if (person.active && person.current_nvtracker_id == det.tracking_id) {
          dwf.fixed_id = person.fixed_id;
          break;
        }
      }
      
      result.push_back(dwf);
    }
    
    return result;
  }

  std::vector<Detection> get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return detections_;
  }

  // 手動登録（手動モード用）
  bool register_by_nvtracker_id(uint64_t nvtracker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 既に登録されているか確認
    for (const auto &person : registered_persons_) {
      if (person.active && person.current_nvtracker_id == nvtracker_id) {
        std::cout << "[api] Already registered: nvtracker=" << nvtracker_id << std::endl;
        return false;
      }
    }
    
    // 空きスロットを探す
    for (auto &person : registered_persons_) {
      if (!person.active) {
        // 検出情報から登録
        for (const auto &det : detections_) {
          if (det.tracking_id == nvtracker_id) {
            auto now = std::chrono::steady_clock::now();
            person.fixed_id = &person - &registered_persons_[0];
            person.current_nvtracker_id = nvtracker_id;
            person.bbox_width = det.width;
            person.bbox_height = det.height;
            person.bbox_left = det.left;
            person.bbox_top = det.top;
            person.stable_bbox_top = det.top;
            person.stable_bbox_height = det.height;
            person.sitting_bbox_height = 0.0f;
            person.prev_bbox_top = det.top;
            person.prev_bbox_height = det.height;
            person.lying_bbox_top = 0.0f;
            person.last_seen = now;
            person.last_update = now;
            person.lying_start = now;
            person.lying_stable = now;
            person.standing_confirmed = now;
            person.sitting_confirmed = now;
            person.head_position_recorded = now;
            person.frame_count = 0;
            person.active = true;
            person.is_lying = (det.width > det.height * 1.8f);
            person.is_sitting = false;
            person.was_standing = !person.is_lying;
            
            std::cout << "[api] Manually registered nvtracker=" << nvtracker_id 
                     << " as Fixed ID " << person.fixed_id << std::endl;
            return true;
          }
        }
      }
    }
    
    return false;
  }
  
  // 固定IDを指定して登録解除（タップで解除）
  bool unregister_by_nvtracker_id(uint64_t nvtracker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto &person : registered_persons_) {
      if (person.active && person.current_nvtracker_id == nvtracker_id) {
        std::cout << "[api] Unregistered nvtracker=" << nvtracker_id 
                 << " (Fixed ID " << person.fixed_id << ")" << std::endl;
        person.active = false;
        return true;
      }
    }
    
    return false;
  }

  // 固定IDを指定して登録解除
  bool unregister_person(int fixed_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (fixed_id < 0 || fixed_id >= MAX_REGISTERED_PERSONS) {
      return false;
    }
    
    if (registered_persons_[fixed_id].active) {
      registered_persons_[fixed_id].active = false;
      std::cout << "[api] Unregistered Fixed ID " << fixed_id << std::endl;
      std::cout.flush();
      return true;
    }
    
    return false;
  }

  // 全員登録解除
  void clear_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &person : registered_persons_) {
      person.active = false;
    }
    std::cout << "[api] Cleared all registrations" << std::endl;
    std::cout.flush();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Detection> detections_;
  std::array<RegisteredPerson, MAX_REGISTERED_PERSONS> registered_persons_;
  std::vector<Alert> alerts_;
};

std::string detections_to_json(const std::vector<DetectionStore::DetectionWithFixedId> &detections) {
  std::ostringstream oss;
  oss << "{\"detections\":[";
  for (size_t i = 0; i < detections.size(); ++i) {
    if (i > 0) oss << ",";
    const auto &d = detections[i].detection;
    const int fixed_id = detections[i].fixed_id;
    oss << "{"
        << "\"nvtracker_id\":" << d.tracking_id << ","
        << "\"fixed_id\":" << fixed_id << ","
        << "\"registered\":" << (fixed_id >= 0 ? "true" : "false") << ","
        << "\"class_id\":" << d.class_id << ","
        << "\"confidence\":" << d.confidence << ","
        << "\"bbox\":{\"left\":" << d.left << ",\"top\":" << d.top
        << ",\"width\":" << d.width << ",\"height\":" << d.height << "}"
        << "}";
  }
  oss << "]}";
  return oss.str();
}

std::string alerts_to_json(const std::vector<Alert> &alerts) {
  std::ostringstream oss;
  oss << "{\"alerts\":[";
  for (size_t i = 0; i < alerts.size(); ++i) {
    if (i > 0) oss << ",";
    const auto &a = alerts[i];
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        a.timestamp.time_since_epoch()).count();
    oss << "{"
        << "\"index\":" << i << ","
        << "\"fixed_id\":" << a.fixed_id << ","
        << "\"type\":" << static_cast<int>(a.type) << ","
        << "\"message\":\"" << a.message << "\","
        << "\"timestamp\":" << ms << ","
        << "\"acknowledged\":" << (a.acknowledged ? "true" : "false")
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
  // std::cout << "[http] MJPEG client disconnected" << std::endl;
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

uint64_t parse_nvtracker_id_from_json(const std::string &json) {
  // Simple JSON parsing: {"nvtracker_id":123}
  size_t pos = json.find("\"nvtracker_id\"");
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

int parse_fixed_id_from_json(const std::string &json) {
  // Simple JSON parsing: {"fixed_id":0}
  size_t pos = json.find("\"fixed_id\"");
  if (pos == std::string::npos) return -1;
  pos = json.find(":", pos);
  if (pos == std::string::npos) return -1;
  pos++;
  while (pos < json.size() && std::isspace(json[pos])) pos++;
  int id = 0;
  bool negative = false;
  if (pos < json.size() && json[pos] == '-') {
    negative = true;
    pos++;
  }
  while (pos < json.size() && std::isdigit(json[pos])) {
    id = id * 10 + (json[pos] - '0');
    pos++;
  }
  return negative ? -id : id;
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
      auto detections = detection_store.get_with_fixed_ids();
      response_body = detections_to_json(detections);
    } else if (method == "GET" && path == "/api/alerts") {
      auto alerts = detection_store.get_alerts();
      response_body = alerts_to_json(alerts);
    } else if (method == "POST" && path == "/api/unregister") {
      // 自動登録モード: nvtracker IDを指定して登録解除
      size_t cl_pos = request.find("Content-Length:");
      size_t content_length = 0;
      if (cl_pos != std::string::npos) {
        content_length = std::atoi(request.c_str() + cl_pos + 15);
      }
      
      size_t body_start = request.find("\r\n\r\n");
      std::string body;
      if (body_start != std::string::npos) {
        body = request.substr(body_start + 4);
        if (body.size() < content_length) {
          body += read_request_body(client_fd, content_length - body.size());
        }
      }
      
      uint64_t nvtracker_id = parse_nvtracker_id_from_json(body);
      bool success = detection_store.unregister_by_nvtracker_id(nvtracker_id);
      
      if (success) {
        response_body = "{\"status\":\"unregistered\",\"nvtracker_id\":" + 
                       std::to_string(nvtracker_id) + "}";
      } else {
        response_body = "{\"status\":\"failed\",\"nvtracker_id\":" + 
                       std::to_string(nvtracker_id) + "}";
      }
    } else if (method == "POST" && path == "/api/clear") {
      detection_store.clear_all();
      response_body = "{\"status\":\"cleared\"}";
    } else if (method == "POST" && path == "/api/acknowledge_alert") {
      // アラート確認
      size_t cl_pos = request.find("Content-Length:");
      size_t content_length = 0;
      if (cl_pos != std::string::npos) {
        content_length = std::atoi(request.c_str() + cl_pos + 15);
      }
      
      size_t body_start = request.find("\r\n\r\n");
      std::string body;
      if (body_start != std::string::npos) {
        body = request.substr(body_start + 4);
        if (body.size() < content_length) {
          body += read_request_body(client_fd, content_length - body.size());
        }
      }
      
      int index = parse_fixed_id_from_json(body.replace(body.find("\"index\""), 7, "\"fixed_id\""));
      detection_store.acknowledge_alert(index);
      response_body = "{\"status\":\"acknowledged\",\"index\":" + std::to_string(index) + "}";
    } else if (method == "POST" && path == "/api/clear_alerts") {
      detection_store.clear_alerts();
      response_body = "{\"status\":\"alerts_cleared\"}";
    } else if (method == "POST" && path == "/api/register") {
      // 手動登録
      size_t cl_pos = request.find("Content-Length:");
      size_t content_length = 0;
      if (cl_pos != std::string::npos) {
        content_length = std::atoi(request.c_str() + cl_pos + 15);
      }
      
      size_t body_start = request.find("\r\n\r\n");
      std::string body;
      if (body_start != std::string::npos) {
        body = request.substr(body_start + 4);
        if (body.size() < content_length) {
          body += read_request_body(client_fd, content_length - body.size());
        }
      }
      
      uint64_t nvtracker_id = parse_nvtracker_id_from_json(body);
      bool success = detection_store.register_by_nvtracker_id(nvtracker_id);
      
      if (success) {
        response_body = "{\"status\":\"registered\",\"nvtracker_id\":" + 
                       std::to_string(nvtracker_id) + "}";
      } else {
        response_body = "{\"status\":\"failed\",\"nvtracker_id\":" + 
                       std::to_string(nvtracker_id) + "}";
      }
    } else if (method == "POST" && path == "/api/toggle_auto_register") {
      // 自動登録モードの切り替え
      bool current = detection_store.get_auto_register();
      detection_store.set_auto_register(!current);
      response_body = "{\"status\":\"toggled\",\"auto_register\":" + 
                     std::string(!current ? "true" : "false") + "}";
    } else if (method == "GET" && path == "/api/config") {
      // 設定取得
      bool auto_reg = detection_store.get_auto_register();
      response_body = "{\"auto_register\":" + std::string(auto_reg ? "true" : "false") + "}";
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
        
        // std::cout << "[http] " << request.substr(0, request.find('\r')) << std::endl;
        
        if (is_api_request(request)) {
          std::thread(serve_api_client, client, request,
                      std::ref(detection_store)).detach();
        } else if (request.find("GET /stream") == 0) {
          std::thread(serve_mjpeg_client, client, std::ref(frame_store)).detach();
        } else if (request.find("GET /debug") == 0) {
          std::thread(serve_html_file, client, 
                      std::string("/workspace/edge-room-monitor/ui/debug.html")).detach();
        } else if (request.find("GET /old") == 0) {
          std::thread(serve_html_file, client, 
                      std::string("/workspace/edge-room-monitor/ui/mjpeg_viewer.html")).detach();
        } else {
          // Default: serve monitoring UI
          std::thread(serve_html_file, client, 
                      std::string("/workspace/edge-room-monitor/ui/monitor.html")).detach();
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
