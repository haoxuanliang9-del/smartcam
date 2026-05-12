#include "i2c_hal.h"
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <cstring>
#include <errno.h>

namespace smartcam {

I2cHal::~I2cHal() {
    close();
}

bool I2cHal::open(const I2cHalConfig& config) {
    config_ = config;

    std::string dev_path = "/dev/i2c-" + std::to_string(config_.bus);
    fd_ = ::open(dev_path.c_str(), O_RDWR);
    if (fd_ < 0) {
        SPDLOG_ERROR("Cannot open I2C device {}: {}", dev_path, strerror(errno));
        return false;
    }

    if (ioctl(fd_, I2C_SLAVE, config_.addr) < 0) {
        SPDLOG_ERROR("Cannot set I2C slave address 0x{:02x}: {}",
                     config_.addr, strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    SPDLOG_INFO("I2C HAL opened: bus={}, addr=0x{:02x}", config_.bus, config_.addr);
    return true;
}

void I2cHal::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        SPDLOG_INFO("I2C HAL closed");
    }
}

bool I2cHal::write(const uint8_t* data, size_t len) {
    if (::write(fd_, data, len) != static_cast<ssize_t>(len)) {
        SPDLOG_ERROR("I2C write failed: {}", strerror(errno));
        return false;
    }
    return true;
}

bool I2cHal::read(uint8_t* data, size_t len) {
    if (::read(fd_, data, len) != static_cast<ssize_t>(len)) {
        SPDLOG_ERROR("I2C read failed: {}", strerror(errno));
        return false;
    }
    return true;
}

bool I2cHal::write_register(uint8_t reg, const uint8_t* data, size_t len) {
    std::vector<uint8_t> buf(len + 1);
    buf[0] = reg;
    std::memcpy(buf.data() + 1, data, len);
    return write(buf.data(), buf.size());
}

bool I2cHal::read_register(uint8_t reg, uint8_t* data, size_t len) {
    if (!write(&reg, 1)) return false;
    return read(data, len);
}

} // namespace smartcam
