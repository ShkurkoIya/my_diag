/// One-shot: capture first N payloads per log-code for layout reverse-engineering.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <qcom/protocol/DiagSourceConfig.h>
#include <qcom/linux/LinuxSource.h>
#include <qcom/io/ScannerEngine.h>
#include <qcom/linux/SourceFactory.h>

int main(int argc, char** argv) {
  const char* dev = argc > 1 ? argv[1] : "/dev/ttyUSB0";
  const int sec = argc > 2 ? std::atoi(argv[2]) : 20;
  const size_t per_code = 12;

  const std::set<QCom::LogCode> want = {
      0xB113, 0xB115, 0xB123, 0xB179, 0xB181, 0xB192, 0xB194, 0xB195, 0xB0CD, 0xB0CB,
      0xB064, 0x4179, 0x41B0, 0xB168, 0xB169, 0xB0E3, 0xB17F, 0xB193, 0xB175, 0xB0C0,
      0xB197, 0xB0C2,
  };

  QCom::DiagSourceConfig cfg{.device_path = dev, .baud_rate = 921600, .init_masks = true};
  QCom::RadioScannerEngine engine(QCom::make_diag_source(std::move(cfg)));

  std::mutex mu;
  std::map<QCom::LogCode, std::vector<std::vector<uint8_t>>> samples;
  std::map<QCom::LogCode, uint64_t> counts;

  engine.set_packet_observer([&](QCom::QualcommPacketView pkt) {
    if (!want.contains(pkt.log_code)) return;
    std::lock_guard lock(mu);
    ++counts[pkt.log_code];
    auto& v = samples[pkt.log_code];
    if (v.size() >= per_code) return;
    v.emplace_back(pkt.payload.begin(), pkt.payload.end());
  });

  if (!engine.start()) {
    std::fprintf(stderr, "start failed on %s\n", dev);
    return 1;
  }
  std::fprintf(stderr, "dumping %ds from %s…\n", sec, dev);
  std::this_thread::sleep_for(std::chrono::seconds(sec));
  engine.stop();

  for (auto& [code, n] : counts) {
    std::printf("# 0x%04X count=%llu samples=%zu\n", code, (unsigned long long)n,
                samples[code].size());
    size_t i = 0;
    for (auto& p : samples[code]) {
      std::printf("0x%04X[%zu] len=%zu ver=%u hex=", code, i++, p.size(),
                  p.empty() ? 0u : p[0]);
      for (uint8_t b : p) std::printf("%02x", b);
      std::printf("\n");
    }
  }
  return 0;
}
