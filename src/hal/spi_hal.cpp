#include "spi_hal.h"
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <fstream>
#include <cstring>
#include <errno.h>

namespace smartcam {

static const char* GPIO_BASE = "/sys/class/gpio";

SpiHal::~SpiHal() { close(); }

bool SpiHal::open(const SpiConfig& config) {
    config_ = config;
    fd_ = ::open(config_.device.c_str(), O_RDWR);
    if (fd_ < 0) {
        SPDLOG_ERROR("Cannot open SPI device {}: {}", config_.device, strerror(errno));
        return false;
    }

    uint8_t mode = config_.mode;
    if (ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0) {
        SPDLOG_ERROR("Cannot set SPI mode: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    uint8_t bits = config_.bits_per_word;
    if (ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        SPDLOG_ERROR("Cannot set SPI bits per word: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &config_.speed_hz) < 0) {
        SPDLOG_ERROR("Cannot set SPI speed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    SPDLOG_INFO("SPI HAL opened: device={}, speed={}Hz, mode={}",
                config_.device, config_.speed_hz, (int)config_.mode);
    return true;
}

void SpiHal::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        SPDLOG_INFO("SPI HAL closed");
    }
}

bool SpiHal::transfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    struct spi_ioc_transfer tr = {};
    tr.tx_buf = reinterpret_cast<uintptr_t>(tx);
    tr.rx_buf = reinterpret_cast<uintptr_t>(rx);
    tr.len = len;
    tr.speed_hz = config_.speed_hz;
    tr.delay_usecs = 0;
    tr.bits_per_word = config_.bits_per_word;
    tr.cs_change = 0;

    if (ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
        SPDLOG_ERROR("SPI transfer failed: {}", strerror(errno));
        return false;
    }
    return true;
}

bool SpiHal::gpio_export(int pin) {
    std::ofstream f(std::string(GPIO_BASE) + "/export");
    if (!f) return false;
    f << pin;
    return true;
}

bool SpiHal::gpio_unexport(int pin) {
    std::ofstream f(std::string(GPIO_BASE) + "/unexport");
    if (!f) return false;
    f << pin;
    return true;
}

bool SpiHal::gpio_set_direction(int pin, const std::string& dir) {
    std::ofstream f(std::string(GPIO_BASE) + "/gpio" + std::to_string(pin) + "/direction");
    if (!f) return false;
    f << dir;
    return true;
}

bool SpiHal::gpio_write(int pin, int value) {
    std::ofstream f(std::string(GPIO_BASE) + "/gpio" + std::to_string(pin) + "/value");
    if (!f) return false;
    f << value;
    return true;
}

} // namespace smartcam
