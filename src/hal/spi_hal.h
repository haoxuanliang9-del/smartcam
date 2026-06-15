#ifndef SMARTCAM_HAL_SPI_HAL_H
#define SMARTCAM_HAL_SPI_HAL_H

#include <cstdint>
#include <string>

namespace smartcam {

struct SpiConfig {
    std::string device = "/dev/spidev0.0";
    uint8_t mode = 0;
    uint32_t speed_hz = 1000000;
    uint8_t bits_per_word = 8;
};

class SpiHal {
public:
    SpiHal() = default;
    ~SpiHal();

    bool open(const SpiConfig& config);
    void close();
    bool is_opened() const { return fd_ >= 0; }

    bool transfer(const uint8_t* tx, uint8_t* rx, size_t len);

    static bool gpio_export(int pin);
    static bool gpio_unexport(int pin);
    static bool gpio_set_direction(int pin, const std::string& dir);
    static bool gpio_write(int pin, int value);

private:
    int fd_ = -1;
    SpiConfig config_;
};

} // namespace smartcam

#endif // SMARTCAM_HAL_SPI_HAL_H
