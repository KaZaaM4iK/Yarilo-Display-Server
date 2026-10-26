// include/platform/drm/device.hpp
#pragma once
#include "core/types.hpp"
#include <string>

namespace platform::drm {

class DeviceDiscovery {
public:
    // Находит системный путь к первичному видеоадаптеру (Linux & FreeBSD)
    static core::Result<std::string> find_primary_gpu_path();
};

} // namespace platform::drm