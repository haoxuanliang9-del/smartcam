#ifndef SMARTCAM_MODULES_DISPLAY_OLED_DISPLAY_H
#define SMARTCAM_MODULES_DISPLAY_OLED_DISPLAY_H

#include "hal/spi_hal.h"
#include "common/config.h"
#include <atomic>
#include <string>
#include <cstdint>

namespace smartcam {

class OledDisplay {
public:
    OledDisplay() = default;
    ~OledDisplay();

    bool init(const DisplayConfig& config);
    void start();
    void stop();

    void update_bitrate(uint32_t kbps);
    void update_sensor(float temp_c, float humidity_pct);

    bool is_running() const { return running_; }

private:
    void display_loop();
    void render();
    void clear_buffer();
    void draw_text(int row, int col, const std::string& text);

    void write_command(uint8_t cmd);
    void write_data(const uint8_t* data, size_t len);
    void reset_hardware();
    void init_ss1306();

    std::string get_wifi_status();

    SpiHal spi_;
    DisplayConfig config_;

    int gpio_dc_ = -1;
    int gpio_rst_ = -1;
    int gpio_cs_ = -1;

    std::atomic<bool> running_{false};
    uint8_t buffer_[128 * 8] = {};

    std::atomic<uint32_t> current_bitrate_{0};
    std::atomic<float> current_temp_{0.0f};
    std::atomic<float> current_humidity_{0.0f};
    std::atomic<uint64_t> last_sensor_update_{0};

    static constexpr int SCREEN_WIDTH = 128;
    static constexpr int FONT_WIDTH = 6;
    static constexpr int FONT_HEIGHT = 8;
    static constexpr int COLS = 21;
    static constexpr int ROWS = 8;

    static const uint8_t FONT_6X8[95][6];
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_DISPLAY_OLED_DISPLAY_H
