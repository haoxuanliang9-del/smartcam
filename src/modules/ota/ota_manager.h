#ifndef SMARTCAM_MODULES_OTA_MANAGER_H
#define SMARTCAM_MODULES_OTA_MANAGER_H

#include "common/config.h"
#include <string>
#include <functional>
#include <atomic>

namespace smartcam {

class OtaManager {
public:
    explicit OtaManager() = default;
    ~OtaManager();

    bool init(const OtaConfig& config);
    void start();
    void stop();

    struct UpdateInfo {
        std::string version;
        std::string download_url;
        std::string sha256;
        uint64_t size;
        std::string description;
    };

    bool check_update(UpdateInfo& info);
    bool download_update(const UpdateInfo& info, const std::string& output_path);
    bool verify_update(const std::string& file_path, const std::string& expected_sha256);
    bool install_update(const std::string& file_path);

    using ProgressCallback = std::function<void(int)>;
    void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }

    bool is_running() const { return running_; }

private:
    void check_loop();
    std::string compute_sha256(const std::string& file_path);

    OtaConfig config_;
    std::atomic<bool> running_{false};
    ProgressCallback progress_cb_;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_OTA_MANAGER_H
