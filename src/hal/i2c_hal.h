#ifndef SMARTCAM_HAL_I2C_HAL_H
#define SMARTCAM_HAL_I2C_HAL_H

#include <cstdint>
#include <string>

namespace smartcam {

struct I2cHalConfig {
    int bus = 1;
    uint8_t addr = 0x38;
};

class I2cHal {
public:
    I2cHal() = default;
    ~I2cHal();

    bool open(const I2cHalConfig& config);
    void close();
    bool is_opened() const { return fd_ >= 0; }

    bool write(const uint8_t* data, size_t len);
    bool read(uint8_t* data, size_t len);

private:
    int fd_ = -1;
    I2cHalConfig config_;
};

} // namespace smartcam

#endif // SMARTCAM_HAL_I2C_HAL_H
