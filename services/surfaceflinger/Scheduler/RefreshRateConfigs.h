/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <android-base/stringprintf.h>

#include <algorithm>
#include <numeric>
#include <type_traits>

#include "DisplayHardware/HWComposer.h"
#include "Scheduler/SchedulerUtils.h"

namespace android::scheduler {

enum class RefreshRateConfigEvent : unsigned { None = 0b0, Changed = 0b1 };

inline RefreshRateConfigEvent operator|(RefreshRateConfigEvent lhs, RefreshRateConfigEvent rhs) {
    using T = std::underlying_type_t<RefreshRateConfigEvent>;
    return static_cast<RefreshRateConfigEvent>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

/**
 * This class is used to encapsulate configuration for refresh rates. It holds information
 * about available refresh rates on the device, and the mapping between the numbers and human
 * readable names.
 */
class RefreshRateConfigs {
public:
    // Enum to indicate which vsync rate to run at. Default is the old 60Hz, and performance
    // is the new 90Hz. Eventually we want to have a way for vendors to map these in the configs.
    enum class RefreshRateType { DEFAULT, PERFORMANCE };

    struct RefreshRate {
        // This config ID corresponds to the position of the config in the vector that is stored
        // on the device.
        int configId;
        // Human readable name of the refresh rate.
        std::string name;
        // Refresh rate in frames per second, rounded to the nearest integer.
        uint32_t fps = 0;
        // Vsync period in nanoseconds.
        nsecs_t vsyncPeriod;
        // Hwc config Id (returned from HWC2::Display::Config::getId())
        hwc2_config_t hwcId;
    };

    // Returns true if this device is doing refresh rate switching. This won't change at runtime.
    bool refreshRateSwitchingSupported() const { return mRefreshRateSwitchingSupported; }

    // Returns the refresh rate map. This map won't be modified at runtime, so it's safe to access
    // from multiple threads. This can only be called if refreshRateSwitching() returns true.
    // TODO(b/122916473): Get this information from configs prepared by vendors, instead of
    // baking them in.
    const std::map<RefreshRateType, RefreshRate>& getRefreshRateMap() const;

    const RefreshRate& getRefreshRateFromType(RefreshRateType type) const;

    std::pair<RefreshRateType, const RefreshRate&> getCurrentRefreshRate() const;

    const RefreshRate& getRefreshRateFromConfigId(int configId) const;

    RefreshRateType getRefreshRateTypeFromHwcConfigId(hwc2_config_t hwcId) const;

    void setCurrentConfig(int config);

    struct InputConfig {
        hwc2_config_t hwcId = 0;
        nsecs_t vsyncPeriod = 0;
    };

    RefreshRateConfigs(bool refreshRateSwitching, const std::vector<InputConfig>& configs,
                       int currentConfig);

    RefreshRateConfigs(bool refreshRateSwitching,
                       const std::vector<std::shared_ptr<const HWC2::Display::Config>>& configs,
                       int currentConfig);

private:
    void init(bool refreshRateSwitching, const std::vector<InputConfig>& configs,
              int currentConfig);
    // Whether this device is doing refresh rate switching or not. This must not change after this
    // object is initialized.
    bool mRefreshRateSwitchingSupported;
    // The list of refresh rates, indexed by display config ID. This must not change after this
    // object is initialized.
    std::vector<RefreshRate> mRefreshRates;
    // The mapping of refresh rate type to RefreshRate. This must not change after this object is
    // initialized.
    std::map<RefreshRateType, RefreshRate> mRefreshRateMap;
    // The ID of the current config. This will change at runtime. This is set by SurfaceFlinger on
    // the main thread, and read by the Scheduler (and other objects) on other threads, so it's
    // atomic.
    std::atomic<int> mCurrentConfig;
};

} // namespace android::scheduler
