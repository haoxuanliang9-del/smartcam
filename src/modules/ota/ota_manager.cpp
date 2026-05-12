#include "ota_manager.h"
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <yaml-cpp/yaml.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>
#include <cstdlib>

namespace smartcam {

OtaManager::~OtaManager() {
    stop();
}

bool OtaManager::init(const OtaConfig& config) {
    config_ = config;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    SPDLOG_INFO("OTA Manager initialized (server={}, interval={}s)",
                config.server_url, config.check_interval_sec);
    return true;
}

void OtaManager::start() {
    if (running_) return;
    running_ = true;
    std::thread(&OtaManager::check_loop, this).detach();
    SPDLOG_INFO("OTA Manager started");
}

void OtaManager::stop() {
    running_ = false;
    SPDLOG_INFO("OTA Manager stopped");
}

void OtaManager::check_loop() {
    while (running_) {
        UpdateInfo info;
        if (check_update(info)) {
            SPDLOG_INFO("Update available: {} (current: {})",
                        info.version, config_.current_version);

            std::string download_path = "/tmp/smartcam_update_" + info.version + ".bin";

            if (download_update(info, download_path)) {
                if (verify_update(download_path, info.sha256)) {
                    SPDLOG_INFO("Update verified, installing...");
                    install_update(download_path);
                } else {
                    SPDLOG_ERROR("Update verification failed");
                    std::remove(download_path.c_str());
                }
            }
        }

        for (uint32_t i = 0; i < config_.check_interval_sec && running_; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* stream = static_cast<std::ofstream*>(userdata);
    stream->write(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t progress_callback(void* clientp, double dltotal, double dlnow,
                                 double ultotal, double ulnow) {
    auto* cb = static_cast<OtaManager::ProgressCallback*>(clientp);
    if (cb && *cb && dltotal > 0) {
        int percent = static_cast<int>((dlnow / dltotal) * 100);
        (*cb)(percent);
    }
    return 0;
}

bool OtaManager::check_update(UpdateInfo& info) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        SPDLOG_ERROR("Cannot initialize CURL");
        return false;
    }

    std::string url = config_.server_url + "/check?version=" + config_.current_version;
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* data) -> size_t {
        auto* resp = static_cast<std::string*>(data);
        resp->append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    if (!config_.certificate.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, config_.certificate.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        SPDLOG_ERROR("Update check failed: {}", curl_easy_strerror(res));
        return false;
    }

    try {
        YAML::Node root = YAML::Load(response);
        if (!root["update_available"] || !root["update_available"].as<bool>()) {
            return false;
        }

        info.version = root["version"].as<std::string>();
        info.download_url = root["download_url"].as<std::string>();
        info.sha256 = root["sha256"].as<std::string>();
        info.size = root["size"].as<uint64_t>();
        if (root["description"]) {
            info.description = root["description"].as<std::string>();
        }

        return true;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to parse update info: {}", e.what());
        return false;
    }
}

bool OtaManager::download_update(const UpdateInfo& info, const std::string& output_path) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        SPDLOG_ERROR("Cannot initialize CURL for download");
        return false;
    }

    std::ofstream outfile(output_path, std::ios::binary);
    if (!outfile) {
        SPDLOG_ERROR("Cannot create output file: {}", output_path);
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, info.download_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outfile);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &progress_cb_);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (!config_.certificate.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, config_.certificate.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    outfile.close();
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        SPDLOG_ERROR("Download failed: {}", curl_easy_strerror(res));
        std::remove(output_path.c_str());
        return false;
    }

    SPDLOG_INFO("Update downloaded to {}", output_path);
    return true;
}

bool OtaManager::verify_update(const std::string& file_path, const std::string& expected_sha256) {
    std::string actual = compute_sha256(file_path);
    bool match = (actual == expected_sha256);

    if (match) {
        SPDLOG_INFO("SHA256 verification passed");
    } else {
        SPDLOG_ERROR("SHA256 mismatch: expected={}, actual={}", expected_sha256, actual);
    }

    return match;
}

bool OtaManager::install_update(const std::string& file_path) {
    SPDLOG_INFO("Installing update from {}...", file_path);

    std::string cmd = "sysupgrade " + file_path;
    int ret = std::system(cmd.c_str());

    if (ret != 0) {
        SPDLOG_ERROR("Update installation failed (ret={})", ret);
        return false;
    }

    SPDLOG_INFO("Update installed, system will reboot");
    return true;
}

std::string OtaManager::compute_sha256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return "";

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    char buffer[8192];
    while (file.read(buffer, sizeof(buffer))) {
        SHA256_Update(&ctx, buffer, file.gcount());
    }
    if (file.gcount() > 0) {
        SHA256_Update(&ctx, buffer, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    std::ostringstream oss;
    for (unsigned char b : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }

    return oss.str();
}

} // namespace smartcam
