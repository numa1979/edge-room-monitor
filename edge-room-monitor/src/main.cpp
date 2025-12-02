#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_running{true};

void signal_handler(int) {
  g_running = false;
}
}

int main() {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::cout << "edge-room-monitor placeholder running.\n"
            << "DeepStream + YOLO 推論パイプライン実装予定。" << std::endl;

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::cout << "Shutting down placeholder app." << std::endl;
  return 0;
}
