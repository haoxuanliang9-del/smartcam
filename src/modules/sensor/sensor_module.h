#ifndef SMARTCAM_MODULES_SENSOR_MODULE_H
#define SMARTCAM_MODULES_SENSOR_MODULE_H

#include "common/config.h"
#include "common/types.h"
#include "hal/i2c_hal.h"
#include "middleware/message_queue.h"
#include <memory>
#include <atomic>

namespace smartcam {

class SensorModule {
public:
    explicit SensorModule(std::shared_ptr<MessageQueue<SensorData>> output_queue);
    ~SensorModule();

    bool init(const SensorConfig& config);
    void start();
    void stop();

    bool read(SensorData& data);
    bool is_running() const { return running_; }

private:
    void sample_loop();
    bool trigger_measurement();
    bool read_measurement(SensorData& data);
    bool check_calibration();
    bool reset_sensor();

    std::shared_ptr<MessageQueue<SensorData>> output_queue_;
    I2cHal i2c_hal_;
    SensorConfig config_;
    std::atomic<bool> running_{false};
    SensorData last_data_{};
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_SENSOR_MODULE_H
