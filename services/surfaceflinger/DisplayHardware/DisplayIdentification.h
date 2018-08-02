/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace android {

using DisplayId = uint64_t;
using DisplayIdentificationData = std::vector<uint8_t>;

// NUL-terminated plug and play ID.
using PnpId = std::array<char, 4>;

struct Edid {
    uint16_t manufacturerId;
    PnpId pnpId;
    std::string_view displayName;
};

bool isEdid(const DisplayIdentificationData&);
std::optional<Edid> parseEdid(const DisplayIdentificationData&);
std::optional<PnpId> getPnpId(uint16_t manufacturerId);

std::optional<DisplayId> generateDisplayId(uint8_t port, const DisplayIdentificationData&);

} // namespace android
