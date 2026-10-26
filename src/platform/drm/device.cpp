// src/platform/drm/device.cpp
#include "platform/drm/device.hpp"
#include "core/error.hpp"
#include <xf86drm.h>
#include <vector>

namespace platform::drm {

core::Result<std::string> DeviceDiscovery::find_primary_gpu_path() {
    constexpr int max_devices = 16;
    drmDevicePtr devices[max_devices];
    
    int num_devices = drmGetDevices2(0, devices, max_devices);
    if (num_devices <= 0) {
        return std::unexpected(core::make_error(core::ServerError::DeviceNotFound));
    }

    std::string primary_path;
    for (int i = 0; i < num_devices; ++i) {
        // Ищем устройство с узлом PRIMARY (доступное для modesetting)
        if (devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)) {
            primary_path = devices[i]->nodes[DRM_NODE_PRIMARY];
            break;
        }
    }

    drmFreeDevices(devices, num_devices);

    if (primary_path.empty()) {
        return std::unexpected(core::make_error(core::ServerError::DeviceNotFound));
    }

    return primary_path;
}

} // namespace platform::drm