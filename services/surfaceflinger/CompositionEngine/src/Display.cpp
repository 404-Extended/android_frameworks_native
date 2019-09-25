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

#include <android-base/stringprintf.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/DisplaySurface.h>
#include <compositionengine/impl/Display.h>
#include <compositionengine/impl/DisplayColorProfile.h>
#include <compositionengine/impl/DumpHelpers.h>
#include <compositionengine/impl/OutputLayer.h>
#include <compositionengine/impl/RenderSurface.h>
#include <utils/Trace.h>

#include "DisplayHardware/HWComposer.h"
#include "DisplayHardware/PowerAdvisor.h"

namespace android::compositionengine::impl {

std::shared_ptr<Display> createDisplay(
        const compositionengine::CompositionEngine& compositionEngine,
        compositionengine::DisplayCreationArgs&& args) {
    return std::make_shared<Display>(compositionEngine, std::move(args));
}

Display::Display(const CompositionEngine& compositionEngine, DisplayCreationArgs&& args)
      : compositionengine::impl::Output(compositionEngine),
        mIsVirtual(args.isVirtual),
        mId(args.displayId),
        mPowerAdvisor(args.powerAdvisor) {
    editState().isSecure = args.isSecure;
}

Display::~Display() = default;

const std::optional<DisplayId>& Display::getId() const {
    return mId;
}

bool Display::isSecure() const {
    return getState().isSecure;
}

bool Display::isVirtual() const {
    return mIsVirtual;
}

void Display::disconnect() {
    if (!mId) {
        return;
    }

    auto& hwc = getCompositionEngine().getHwComposer();
    hwc.disconnectDisplay(*mId);
    mId.reset();
}

void Display::setColorTransform(const mat4& transform) {
    Output::setColorTransform(transform);

    auto& hwc = getCompositionEngine().getHwComposer();
    status_t result = hwc.setColorTransform(*mId, transform);
    ALOGE_IF(result != NO_ERROR, "Failed to set color transform on display \"%s\": %d",
             mId ? to_string(*mId).c_str() : "", result);
}

void Display::setColorMode(ui::ColorMode mode, ui::Dataspace dataspace,
                           ui::RenderIntent renderIntent,
                           ui::Dataspace colorSpaceAgnosticDataspace) {
    ui::Dataspace targetDataspace =
            getDisplayColorProfile()->getTargetDataspace(mode, dataspace,
                                                         colorSpaceAgnosticDataspace);

    if (mode == getState().colorMode && dataspace == getState().dataspace &&
        renderIntent == getState().renderIntent && targetDataspace == getState().targetDataspace) {
        return;
    }

    if (mIsVirtual) {
        ALOGW("%s: Invalid operation on virtual display", __FUNCTION__);
        return;
    }

    Output::setColorMode(mode, dataspace, renderIntent, colorSpaceAgnosticDataspace);

    auto& hwc = getCompositionEngine().getHwComposer();
    hwc.setActiveColorMode(*mId, mode, renderIntent);
}

void Display::dump(std::string& out) const {
    using android::base::StringAppendF;

    StringAppendF(&out, "   Composition Display State: [\"%s\"]", getName().c_str());

    out.append("\n   ");

    dumpVal(out, "isVirtual", mIsVirtual);
    if (mId) {
        dumpVal(out, "hwcId", to_string(*mId));
    } else {
        StringAppendF(&out, "no hwcId, ");
    }

    out.append("\n");

    Output::dumpBase(out);
}

void Display::createDisplayColorProfile(DisplayColorProfileCreationArgs&& args) {
    setDisplayColorProfile(compositionengine::impl::createDisplayColorProfile(std::move(args)));
}

void Display::createRenderSurface(RenderSurfaceCreationArgs&& args) {
    setRenderSurface(compositionengine::impl::createRenderSurface(getCompositionEngine(), *this,
                                                                  std::move(args)));
}

void Display::chooseCompositionStrategy() {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    // Default to the base settings -- client composition only.
    Output::chooseCompositionStrategy();

    // If we don't have a HWC display, then we are done
    if (!mId) {
        return;
    }

    // Get any composition changes requested by the HWC device, and apply them.
    std::optional<android::HWComposer::DeviceRequestedChanges> changes;
    auto& hwc = getCompositionEngine().getHwComposer();
    if (status_t result = hwc.getDeviceCompositionChanges(*mId, anyLayersRequireClientComposition(),
                                                          &changes);
        result != NO_ERROR) {
        ALOGE("chooseCompositionStrategy failed for %s: %d (%s)", getName().c_str(), result,
              strerror(-result));
        return;
    }
    if (changes) {
        applyChangedTypesToLayers(changes->changedTypes);
        applyDisplayRequests(changes->displayRequests);
        applyLayerRequestsToLayers(changes->layerRequests);
    }

    // Determine what type of composition we are doing from the final state
    auto& state = editState();
    state.usesClientComposition = anyLayersRequireClientComposition();
    state.usesDeviceComposition = !allLayersRequireClientComposition();
}

bool Display::getSkipColorTransform() const {
    if (!mId) {
        return false;
    }

    auto& hwc = getCompositionEngine().getHwComposer();
    return hwc.hasDisplayCapability(*mId, HWC2::DisplayCapability::SkipClientColorTransform);
}

bool Display::anyLayersRequireClientComposition() const {
    const auto& layers = getOutputLayersOrderedByZ();
    return std::any_of(layers.cbegin(), layers.cend(),
                       [](const auto& layer) { return layer->requiresClientComposition(); });
}

bool Display::allLayersRequireClientComposition() const {
    const auto& layers = getOutputLayersOrderedByZ();
    return std::all_of(layers.cbegin(), layers.cend(),
                       [](const auto& layer) { return layer->requiresClientComposition(); });
}

void Display::applyChangedTypesToLayers(const ChangedTypes& changedTypes) {
    if (changedTypes.empty()) {
        return;
    }

    for (auto& layer : getOutputLayersOrderedByZ()) {
        auto hwcLayer = layer->getHwcLayer();
        if (!hwcLayer) {
            continue;
        }

        if (auto it = changedTypes.find(hwcLayer); it != changedTypes.end()) {
            layer->applyDeviceCompositionTypeChange(
                    static_cast<Hwc2::IComposerClient::Composition>(it->second));
        }
    }
}

void Display::applyDisplayRequests(const DisplayRequests& displayRequests) {
    auto& state = editState();
    state.flipClientTarget = (static_cast<uint32_t>(displayRequests) &
                              static_cast<uint32_t>(HWC2::DisplayRequest::FlipClientTarget)) != 0;
    // Note: HWC2::DisplayRequest::WriteClientTargetToOutput is currently ignored.
}

void Display::applyLayerRequestsToLayers(const LayerRequests& layerRequests) {
    for (auto& layer : getOutputLayersOrderedByZ()) {
        layer->prepareForDeviceLayerRequests();

        auto hwcLayer = layer->getHwcLayer();
        if (!hwcLayer) {
            continue;
        }

        if (auto it = layerRequests.find(hwcLayer); it != layerRequests.end()) {
            layer->applyDeviceLayerRequest(
                    static_cast<Hwc2::IComposerClient::LayerRequest>(it->second));
        }
    }
}

compositionengine::Output::FrameFences Display::presentAndGetFrameFences() {
    auto result = impl::Output::presentAndGetFrameFences();

    if (!mId) {
        return result;
    }

    auto& hwc = getCompositionEngine().getHwComposer();
    hwc.presentAndGetReleaseFences(*mId);

    result.presentFence = hwc.getPresentFence(*mId);

    // TODO(b/121291683): Change HWComposer call to return entire map
    for (const auto& layer : getOutputLayersOrderedByZ()) {
        auto hwcLayer = layer->getHwcLayer();
        if (!hwcLayer) {
            continue;
        }

        result.layerFences.emplace(hwcLayer, hwc.getLayerReleaseFence(*mId, hwcLayer));
    }

    hwc.clearReleaseFences(*mId);

    return result;
}

void Display::setExpensiveRenderingExpected(bool enabled) {
    Output::setExpensiveRenderingExpected(enabled);

    if (mPowerAdvisor && mId) {
        mPowerAdvisor->setExpensiveRenderingExpected(*mId, enabled);
    }
}

} // namespace android::compositionengine::impl
