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

#include <thread>

#include <android-base/stringprintf.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/Layer.h>
#include <compositionengine/LayerFE.h>
#include <compositionengine/LayerFECompositionState.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/impl/Output.h>
#include <compositionengine/impl/OutputLayer.h>
#include <renderengine/DisplaySettings.h>
#include <renderengine/RenderEngine.h>
#include <ui/DebugUtils.h>
#include <ui/HdrCapabilities.h>
#include <utils/Trace.h>

#include "TracedOrdinal.h"

namespace android::compositionengine {

Output::~Output() = default;

namespace impl {

namespace {

template <typename T>
class Reversed {
public:
    explicit Reversed(const T& container) : mContainer(container) {}
    auto begin() { return mContainer.rbegin(); }
    auto end() { return mContainer.rend(); }

private:
    const T& mContainer;
};

// Helper for enumerating over a container in reverse order
template <typename T>
Reversed<T> reversed(const T& c) {
    return Reversed<T>(c);
}

} // namespace

Output::Output(const CompositionEngine& compositionEngine)
      : mCompositionEngine(compositionEngine) {}

Output::~Output() = default;

const CompositionEngine& Output::getCompositionEngine() const {
    return mCompositionEngine;
}

bool Output::isValid() const {
    return mDisplayColorProfile && mDisplayColorProfile->isValid() && mRenderSurface &&
            mRenderSurface->isValid();
}

const std::string& Output::getName() const {
    return mName;
}

void Output::setName(const std::string& name) {
    mName = name;
}

void Output::setCompositionEnabled(bool enabled) {
    if (mState.isEnabled == enabled) {
        return;
    }

    mState.isEnabled = enabled;
    dirtyEntireOutput();
}

void Output::setProjection(const ui::Transform& transform, int32_t orientation, const Rect& frame,
                           const Rect& viewport, const Rect& scissor, bool needsFiltering) {
    mState.transform = transform;
    mState.orientation = orientation;
    mState.scissor = scissor;
    mState.frame = frame;
    mState.viewport = viewport;
    mState.needsFiltering = needsFiltering;

    dirtyEntireOutput();
}

// TODO(b/121291683): Rename setSize() once more is moved.
void Output::setBounds(const ui::Size& size) {
    mRenderSurface->setDisplaySize(size);
    // TODO(b/121291683): Rename mState.size once more is moved.
    mState.bounds = Rect(mRenderSurface->getSize());

    dirtyEntireOutput();
}

void Output::setLayerStackFilter(uint32_t layerStackId, bool isInternal) {
    mState.layerStackId = layerStackId;
    mState.layerStackInternal = isInternal;

    dirtyEntireOutput();
}

void Output::setColorTransform(const compositionengine::CompositionRefreshArgs& args) {
    if (!args.colorTransformMatrix || mState.colorTransformMatrix == *args.colorTransformMatrix) {
        return;
    }

    mState.colorTransformMatrix = *args.colorTransformMatrix;

    dirtyEntireOutput();
}

void Output::setColorProfile(const ColorProfile& colorProfile) {
    const ui::Dataspace targetDataspace =
            getDisplayColorProfile()->getTargetDataspace(colorProfile.mode, colorProfile.dataspace,
                                                         colorProfile.colorSpaceAgnosticDataspace);

    if (mState.colorMode == colorProfile.mode && mState.dataspace == colorProfile.dataspace &&
        mState.renderIntent == colorProfile.renderIntent &&
        mState.targetDataspace == targetDataspace) {
        return;
    }

    mState.colorMode = colorProfile.mode;
    mState.dataspace = colorProfile.dataspace;
    mState.renderIntent = colorProfile.renderIntent;
    mState.targetDataspace = targetDataspace;

    mRenderSurface->setBufferDataspace(colorProfile.dataspace);

    ALOGV("Set active color mode: %s (%d), active render intent: %s (%d)",
          decodeColorMode(colorProfile.mode).c_str(), colorProfile.mode,
          decodeRenderIntent(colorProfile.renderIntent).c_str(), colorProfile.renderIntent);

    dirtyEntireOutput();
}

void Output::dump(std::string& out) const {
    using android::base::StringAppendF;

    StringAppendF(&out, "   Composition Output State: [\"%s\"]", mName.c_str());

    out.append("\n   ");

    dumpBase(out);
}

void Output::dumpBase(std::string& out) const {
    mState.dump(out);

    if (mDisplayColorProfile) {
        mDisplayColorProfile->dump(out);
    } else {
        out.append("    No display color profile!\n");
    }

    if (mRenderSurface) {
        mRenderSurface->dump(out);
    } else {
        out.append("    No render surface!\n");
    }

    android::base::StringAppendF(&out, "\n   %zu Layers\b", mOutputLayersOrderedByZ.size());
    for (const auto& outputLayer : mOutputLayersOrderedByZ) {
        if (!outputLayer) {
            continue;
        }
        outputLayer->dump(out);
    }
}

compositionengine::DisplayColorProfile* Output::getDisplayColorProfile() const {
    return mDisplayColorProfile.get();
}

void Output::setDisplayColorProfile(std::unique_ptr<compositionengine::DisplayColorProfile> mode) {
    mDisplayColorProfile = std::move(mode);
}

const Output::ReleasedLayers& Output::getReleasedLayersForTest() const {
    return mReleasedLayers;
}

void Output::setDisplayColorProfileForTest(
        std::unique_ptr<compositionengine::DisplayColorProfile> mode) {
    mDisplayColorProfile = std::move(mode);
}

compositionengine::RenderSurface* Output::getRenderSurface() const {
    return mRenderSurface.get();
}

void Output::setRenderSurface(std::unique_ptr<compositionengine::RenderSurface> surface) {
    mRenderSurface = std::move(surface);
    mState.bounds = Rect(mRenderSurface->getSize());

    dirtyEntireOutput();
}

void Output::setRenderSurfaceForTest(std::unique_ptr<compositionengine::RenderSurface> surface) {
    mRenderSurface = std::move(surface);
}

const OutputCompositionState& Output::getState() const {
    return mState;
}

OutputCompositionState& Output::editState() {
    return mState;
}

Region Output::getDirtyRegion(bool repaintEverything) const {
    Region dirty(mState.viewport);
    if (!repaintEverything) {
        dirty.andSelf(mState.dirtyRegion);
    }
    return dirty;
}

bool Output::belongsInOutput(std::optional<uint32_t> layerStackId, bool internalOnly) const {
    // The layerStackId's must match, and also the layer must not be internal
    // only when not on an internal output.
    return layerStackId && (*layerStackId == mState.layerStackId) &&
            (!internalOnly || mState.layerStackInternal);
}

bool Output::belongsInOutput(const compositionengine::Layer* layer) const {
    if (!layer) {
        return false;
    }

    const auto& layerFEState = layer->getFEState();
    return belongsInOutput(layerFEState.layerStackId, layerFEState.internalOnly);
}

compositionengine::OutputLayer* Output::getOutputLayerForLayer(
        compositionengine::Layer* layer) const {
    for (const auto& outputLayer : mOutputLayersOrderedByZ) {
        if (outputLayer && &outputLayer->getLayer() == layer) {
            return outputLayer.get();
        }
    }
    return nullptr;
}

std::unique_ptr<compositionengine::OutputLayer> Output::takeOutputLayerForLayer(
        compositionengine::Layer* layer) {
    // Removes the outputLayer from mOutputLayersorderedByZ and transfers ownership to the caller.
    for (auto& outputLayer : mOutputLayersOrderedByZ) {
        if (outputLayer && &outputLayer->getLayer() == layer) {
            return std::move(outputLayer);
        }
    }
    return nullptr;
}

std::unique_ptr<compositionengine::OutputLayer> Output::getOrCreateOutputLayer(
        std::shared_ptr<compositionengine::Layer> layer, sp<compositionengine::LayerFE> layerFE) {
    auto result = takeOutputLayerForLayer(layer.get());
    if (!result) {
        result = createOutputLayer(layer, layerFE);
    }
    return result;
}

std::unique_ptr<compositionengine::OutputLayer> Output::createOutputLayer(
        const std::shared_ptr<compositionengine::Layer>& layer,
        const sp<compositionengine::LayerFE>& layerFE) const {
    return impl::createOutputLayer(*this, layer, layerFE);
}

void Output::setOutputLayersOrderedByZ(OutputLayers&& layers) {
    mOutputLayersOrderedByZ = std::move(layers);
}

const Output::OutputLayers& Output::getOutputLayersOrderedByZ() const {
    return mOutputLayersOrderedByZ;
}

void Output::setReleasedLayers(Output::ReleasedLayers&& layers) {
    mReleasedLayers = std::move(layers);
}

Output::ReleasedLayers Output::takeReleasedLayers() {
    return std::move(mReleasedLayers);
}

void Output::prepare(const compositionengine::CompositionRefreshArgs& refreshArgs,
                     LayerFESet& geomSnapshots) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    rebuildLayerStacks(refreshArgs, geomSnapshots);
}

void Output::present(const compositionengine::CompositionRefreshArgs& refreshArgs) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    updateColorProfile(refreshArgs);
    updateAndWriteCompositionState(refreshArgs);
    setColorTransform(refreshArgs);
    beginFrame();
    prepareFrame();
    devOptRepaintFlash(refreshArgs);
    finishFrame(refreshArgs);
    postFramebuffer();
}

void Output::rebuildLayerStacks(const compositionengine::CompositionRefreshArgs& refreshArgs,
                                LayerFESet& layerFESet) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    // Do nothing if this output is not enabled or there is no need to perform this update
    if (!mState.isEnabled || CC_LIKELY(!refreshArgs.updatingOutputGeometryThisFrame)) {
        return;
    }

    // Process the layers to determine visibility and coverage
    compositionengine::Output::CoverageState coverage{layerFESet};
    collectVisibleLayers(refreshArgs, coverage);

    // Compute the resulting coverage for this output, and store it for later
    const ui::Transform& tr = mState.transform;
    Region undefinedRegion{mState.bounds};
    undefinedRegion.subtractSelf(tr.transform(coverage.aboveOpaqueLayers));

    mState.undefinedRegion = undefinedRegion;
    mState.dirtyRegion.orSelf(coverage.dirtyRegion);
}

void Output::collectVisibleLayers(const compositionengine::CompositionRefreshArgs& refreshArgs,
                                  compositionengine::Output::CoverageState& coverage) {
    // We build up a list of all layers that are going to be visible in the new
    // frame.
    compositionengine::Output::OutputLayers newLayersSortedByZ;

    // Evaluate the layers from front to back to determine what is visible. This
    // also incrementally calculates the coverage information for each layer as
    // well as the entire output.
    for (auto& layer : reversed(refreshArgs.layers)) {
        // Incrementally process the coverage for each layer, obtaining an
        // optional outputLayer if the layer is visible.
        auto outputLayer = getOutputLayerIfVisible(layer, coverage);
        if (outputLayer) {
            newLayersSortedByZ.emplace_back(std::move(outputLayer));
        }

        // TODO(b/121291683): Stop early if the output is completely covered and
        // no more layers could even be visible underneath the ones on top.
    }

    // Since we walked the layers in reverse order, we need to reverse
    // newLayersSortedByZ to get the back-to-front ordered list of layers.
    std::reverse(newLayersSortedByZ.begin(), newLayersSortedByZ.end());

    // Generate a simple Z-order values to each visible output layer
    uint32_t zOrder = 0;
    for (auto& outputLayer : newLayersSortedByZ) {
        outputLayer->editState().z = zOrder++;
    }

    setReleasedLayers(refreshArgs);

    mOutputLayersOrderedByZ = std::move(newLayersSortedByZ);
}

std::unique_ptr<compositionengine::OutputLayer> Output::getOutputLayerIfVisible(
        std::shared_ptr<compositionengine::Layer> layer,
        compositionengine::Output::CoverageState& coverage) {
    // Note: Converts a wp<LayerFE> to a sp<LayerFE>
    auto layerFE = layer->getLayerFE();
    if (layerFE == nullptr) {
        return nullptr;
    }

    // Ensure we have a snapshot of the basic geometry layer state. Limit the
    // snapshots to once per frame for each candidate layer, as layers may
    // appear on multiple outputs.
    if (!coverage.latchedLayers.count(layerFE)) {
        coverage.latchedLayers.insert(layerFE);
        layerFE->latchCompositionState(layer->editFEState(),
                                       compositionengine::LayerFE::StateSubset::BasicGeometry);
    }

    // Obtain a read-only reference to the front-end layer state
    const auto& layerFEState = layer->getFEState();

    // Only consider the layers on the given layer stack
    if (!belongsInOutput(layer.get())) {
        return nullptr;
    }

    /*
     * opaqueRegion: area of a surface that is fully opaque.
     */
    Region opaqueRegion;

    /*
     * visibleRegion: area of a surface that is visible on screen and not fully
     * transparent. This is essentially the layer's footprint minus the opaque
     * regions above it. Areas covered by a translucent surface are considered
     * visible.
     */
    Region visibleRegion;

    /*
     * coveredRegion: area of a surface that is covered by all visible regions
     * above it (which includes the translucent areas).
     */
    Region coveredRegion;

    /*
     * transparentRegion: area of a surface that is hinted to be completely
     * transparent. This is only used to tell when the layer has no visible non-
     * transparent regions and can be removed from the layer list. It does not
     * affect the visibleRegion of this layer or any layers beneath it. The hint
     * may not be correct if apps don't respect the SurfaceView restrictions
     * (which, sadly, some don't).
     */
    Region transparentRegion;

    // handle hidden surfaces by setting the visible region to empty
    if (CC_UNLIKELY(!layerFEState.isVisible)) {
        return nullptr;
    }

    const ui::Transform& tr = layerFEState.geomLayerTransform;

    // Get the visible region
    // TODO(b/121291683): Is it worth creating helper methods on LayerFEState
    // for computations like this?
    visibleRegion.set(Rect(tr.transform(layerFEState.geomLayerBounds)));

    if (visibleRegion.isEmpty()) {
        return nullptr;
    }

    // Remove the transparent area from the visible region
    if (!layerFEState.isOpaque) {
        if (tr.preserveRects()) {
            // transform the transparent region
            transparentRegion = tr.transform(layerFEState.transparentRegionHint);
        } else {
            // transformation too complex, can't do the
            // transparent region optimization.
            transparentRegion.clear();
        }
    }

    // compute the opaque region
    const int32_t layerOrientation = tr.getOrientation();
    if (layerFEState.isOpaque && ((layerOrientation & ui::Transform::ROT_INVALID) == 0)) {
        // If we one of the simple category of transforms (0/90/180/270 rotation
        // + any flip), then the opaque region is the layer's footprint.
        // Otherwise we don't try and compute the opaque region since there may
        // be errors at the edges, and we treat the entire layer as
        // translucent.
        opaqueRegion = visibleRegion;
    }

    // Clip the covered region to the visible region
    coveredRegion = coverage.aboveCoveredLayers.intersect(visibleRegion);

    // Update accumAboveCoveredLayers for next (lower) layer
    coverage.aboveCoveredLayers.orSelf(visibleRegion);

    // subtract the opaque region covered by the layers above us
    visibleRegion.subtractSelf(coverage.aboveOpaqueLayers);

    if (visibleRegion.isEmpty()) {
        return nullptr;
    }

    // Get coverage information for the layer as previously displayed,
    // also taking over ownership from mOutputLayersorderedByZ.
    auto prevOutputLayer = takeOutputLayerForLayer(layer.get());

    //  Get coverage information for the layer as previously displayed
    // TODO(b/121291683): Define kEmptyRegion as a constant in Region.h
    const Region kEmptyRegion;
    const Region& oldVisibleRegion =
            prevOutputLayer ? prevOutputLayer->getState().visibleRegion : kEmptyRegion;
    const Region& oldCoveredRegion =
            prevOutputLayer ? prevOutputLayer->getState().coveredRegion : kEmptyRegion;

    // compute this layer's dirty region
    Region dirty;
    if (layerFEState.contentDirty) {
        // we need to invalidate the whole region
        dirty = visibleRegion;
        // as well, as the old visible region
        dirty.orSelf(oldVisibleRegion);
    } else {
        /* compute the exposed region:
         *   the exposed region consists of two components:
         *   1) what's VISIBLE now and was COVERED before
         *   2) what's EXPOSED now less what was EXPOSED before
         *
         * note that (1) is conservative, we start with the whole visible region
         * but only keep what used to be covered by something -- which mean it
         * may have been exposed.
         *
         * (2) handles areas that were not covered by anything but got exposed
         * because of a resize.
         *
         */
        const Region newExposed = visibleRegion - coveredRegion;
        const Region oldExposed = oldVisibleRegion - oldCoveredRegion;
        dirty = (visibleRegion & oldCoveredRegion) | (newExposed - oldExposed);
    }
    dirty.subtractSelf(coverage.aboveOpaqueLayers);

    // accumulate to the screen dirty region
    coverage.dirtyRegion.orSelf(dirty);

    // Update accumAboveOpaqueLayers for next (lower) layer
    coverage.aboveOpaqueLayers.orSelf(opaqueRegion);

    // Compute the visible non-transparent region
    Region visibleNonTransparentRegion = visibleRegion.subtract(transparentRegion);

    // Peform the final check to see if this layer is visible on this output
    // TODO(b/121291683): Why does this not use visibleRegion? (see outputSpaceVisibleRegion below)
    Region drawRegion(mState.transform.transform(visibleNonTransparentRegion));
    drawRegion.andSelf(mState.bounds);
    if (drawRegion.isEmpty()) {
        return nullptr;
    }

    // The layer is visible. Either reuse the existing outputLayer if we have
    // one, or create a new one if we do not.
    std::unique_ptr<compositionengine::OutputLayer> result =
            prevOutputLayer ? std::move(prevOutputLayer) : createOutputLayer(layer, layerFE);

    // Store the layer coverage information into the layer state as some of it
    // is useful later.
    auto& outputLayerState = result->editState();
    outputLayerState.visibleRegion = visibleRegion;
    outputLayerState.visibleNonTransparentRegion = visibleNonTransparentRegion;
    outputLayerState.coveredRegion = coveredRegion;
    outputLayerState.outputSpaceVisibleRegion =
            mState.transform.transform(outputLayerState.visibleRegion.intersect(mState.viewport));

    return result;
}

void Output::setReleasedLayers(const compositionengine::CompositionRefreshArgs&) {
    // The base class does nothing with this call.
}

void Output::updateLayerStateFromFE(const CompositionRefreshArgs& args) const {
    for (auto& layer : mOutputLayersOrderedByZ) {
        layer->getLayerFE().latchCompositionState(layer->getLayer().editFEState(),
                                                  args.updatingGeometryThisFrame
                                                          ? LayerFE::StateSubset::GeometryAndContent
                                                          : LayerFE::StateSubset::Content);
    }
}

void Output::updateAndWriteCompositionState(
        const compositionengine::CompositionRefreshArgs& refreshArgs) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    for (auto& layer : mOutputLayersOrderedByZ) {
        if (refreshArgs.devOptForceClientComposition) {
            layer->editState().forceClientComposition = true;
        }

        layer->updateCompositionState(refreshArgs.updatingGeometryThisFrame);

        // Send the updated state to the HWC, if appropriate.
        layer->writeStateToHWC(refreshArgs.updatingGeometryThisFrame);
    }
}

void Output::updateColorProfile(const compositionengine::CompositionRefreshArgs& refreshArgs) {
    setColorProfile(pickColorProfile(refreshArgs));
}

// Returns a data space that fits all visible layers.  The returned data space
// can only be one of
//  - Dataspace::SRGB (use legacy dataspace and let HWC saturate when colors are enhanced)
//  - Dataspace::DISPLAY_P3
//  - Dataspace::DISPLAY_BT2020
// The returned HDR data space is one of
//  - Dataspace::UNKNOWN
//  - Dataspace::BT2020_HLG
//  - Dataspace::BT2020_PQ
ui::Dataspace Output::getBestDataspace(ui::Dataspace* outHdrDataSpace,
                                       bool* outIsHdrClientComposition) const {
    ui::Dataspace bestDataSpace = ui::Dataspace::V0_SRGB;
    *outHdrDataSpace = ui::Dataspace::UNKNOWN;

    for (const auto& layer : mOutputLayersOrderedByZ) {
        switch (layer->getLayer().getFEState().dataspace) {
            case ui::Dataspace::V0_SCRGB:
            case ui::Dataspace::V0_SCRGB_LINEAR:
            case ui::Dataspace::BT2020:
            case ui::Dataspace::BT2020_ITU:
            case ui::Dataspace::BT2020_LINEAR:
            case ui::Dataspace::DISPLAY_BT2020:
                bestDataSpace = ui::Dataspace::DISPLAY_BT2020;
                break;
            case ui::Dataspace::DISPLAY_P3:
                bestDataSpace = ui::Dataspace::DISPLAY_P3;
                break;
            case ui::Dataspace::BT2020_PQ:
            case ui::Dataspace::BT2020_ITU_PQ:
                bestDataSpace = ui::Dataspace::DISPLAY_P3;
                *outHdrDataSpace = ui::Dataspace::BT2020_PQ;
                *outIsHdrClientComposition = layer->getLayer().getFEState().forceClientComposition;
                break;
            case ui::Dataspace::BT2020_HLG:
            case ui::Dataspace::BT2020_ITU_HLG:
                bestDataSpace = ui::Dataspace::DISPLAY_P3;
                // When there's mixed PQ content and HLG content, we set the HDR
                // data space to be BT2020_PQ and convert HLG to PQ.
                if (*outHdrDataSpace == ui::Dataspace::UNKNOWN) {
                    *outHdrDataSpace = ui::Dataspace::BT2020_HLG;
                }
                break;
            default:
                break;
        }
    }

    return bestDataSpace;
}

compositionengine::Output::ColorProfile Output::pickColorProfile(
        const compositionengine::CompositionRefreshArgs& refreshArgs) const {
    if (refreshArgs.outputColorSetting == OutputColorSetting::kUnmanaged) {
        return ColorProfile{ui::ColorMode::NATIVE, ui::Dataspace::UNKNOWN,
                            ui::RenderIntent::COLORIMETRIC,
                            refreshArgs.colorSpaceAgnosticDataspace};
    }

    ui::Dataspace hdrDataSpace;
    bool isHdrClientComposition = false;
    ui::Dataspace bestDataSpace = getBestDataspace(&hdrDataSpace, &isHdrClientComposition);

    switch (refreshArgs.forceOutputColorMode) {
        case ui::ColorMode::SRGB:
            bestDataSpace = ui::Dataspace::V0_SRGB;
            break;
        case ui::ColorMode::DISPLAY_P3:
            bestDataSpace = ui::Dataspace::DISPLAY_P3;
            break;
        default:
            break;
    }

    // respect hdrDataSpace only when there is no legacy HDR support
    const bool isHdr = hdrDataSpace != ui::Dataspace::UNKNOWN &&
            !mDisplayColorProfile->hasLegacyHdrSupport(hdrDataSpace) && !isHdrClientComposition;
    if (isHdr) {
        bestDataSpace = hdrDataSpace;
    }

    ui::RenderIntent intent;
    switch (refreshArgs.outputColorSetting) {
        case OutputColorSetting::kManaged:
        case OutputColorSetting::kUnmanaged:
            intent = isHdr ? ui::RenderIntent::TONE_MAP_COLORIMETRIC
                           : ui::RenderIntent::COLORIMETRIC;
            break;
        case OutputColorSetting::kEnhanced:
            intent = isHdr ? ui::RenderIntent::TONE_MAP_ENHANCE : ui::RenderIntent::ENHANCE;
            break;
        default: // vendor display color setting
            intent = static_cast<ui::RenderIntent>(refreshArgs.outputColorSetting);
            break;
    }

    ui::ColorMode outMode;
    ui::Dataspace outDataSpace;
    ui::RenderIntent outRenderIntent;
    mDisplayColorProfile->getBestColorMode(bestDataSpace, intent, &outDataSpace, &outMode,
                                           &outRenderIntent);

    return ColorProfile{outMode, outDataSpace, outRenderIntent,
                        refreshArgs.colorSpaceAgnosticDataspace};
}

void Output::beginFrame() {
    const bool dirty = !getDirtyRegion(false).isEmpty();
    const bool empty = mOutputLayersOrderedByZ.empty();
    const bool wasEmpty = !mState.lastCompositionHadVisibleLayers;

    // If nothing has changed (!dirty), don't recompose.
    // If something changed, but we don't currently have any visible layers,
    //   and didn't when we last did a composition, then skip it this time.
    // The second rule does two things:
    // - When all layers are removed from a display, we'll emit one black
    //   frame, then nothing more until we get new layers.
    // - When a display is created with a private layer stack, we won't
    //   emit any black frames until a layer is added to the layer stack.
    const bool mustRecompose = dirty && !(empty && wasEmpty);

    const char flagPrefix[] = {'-', '+'};
    static_cast<void>(flagPrefix);
    ALOGV_IF("%s: %s composition for %s (%cdirty %cempty %cwasEmpty)", __FUNCTION__,
             mustRecompose ? "doing" : "skipping", getName().c_str(), flagPrefix[dirty],
             flagPrefix[empty], flagPrefix[wasEmpty]);

    mRenderSurface->beginFrame(mustRecompose);

    if (mustRecompose) {
        mState.lastCompositionHadVisibleLayers = !empty;
    }
}

void Output::prepareFrame() {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    if (!mState.isEnabled) {
        return;
    }

    chooseCompositionStrategy();

    mRenderSurface->prepareFrame(mState.usesClientComposition, mState.usesDeviceComposition);
}

void Output::devOptRepaintFlash(const compositionengine::CompositionRefreshArgs& refreshArgs) {
    if (CC_LIKELY(!refreshArgs.devOptFlashDirtyRegionsDelay)) {
        return;
    }

    if (mState.isEnabled) {
        // transform the dirty region into this screen's coordinate space
        const Region dirtyRegion = getDirtyRegion(refreshArgs.repaintEverything);
        if (!dirtyRegion.isEmpty()) {
            base::unique_fd readyFence;
            // redraw the whole screen
            static_cast<void>(composeSurfaces(dirtyRegion));

            mRenderSurface->queueBuffer(std::move(readyFence));
        }
    }

    postFramebuffer();

    std::this_thread::sleep_for(*refreshArgs.devOptFlashDirtyRegionsDelay);

    prepareFrame();
}

void Output::finishFrame(const compositionengine::CompositionRefreshArgs&) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    if (!mState.isEnabled) {
        return;
    }

    // Repaint the framebuffer (if needed), getting the optional fence for when
    // the composition completes.
    auto optReadyFence = composeSurfaces(Region::INVALID_REGION);
    if (!optReadyFence) {
        return;
    }

    // swap buffers (presentation)
    mRenderSurface->queueBuffer(std::move(*optReadyFence));
}

std::optional<base::unique_fd> Output::composeSurfaces(const Region& debugRegion) {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    const TracedOrdinal<bool> hasClientComposition = {"hasClientComposition",
                                                      mState.usesClientComposition};
    base::unique_fd readyFence;

    if (!hasClientComposition) {
        return readyFence;
    }

    ALOGV("hasClientComposition");

    auto& renderEngine = mCompositionEngine.getRenderEngine();
    const bool supportsProtectedContent = renderEngine.supportsProtectedContent();

    renderengine::DisplaySettings clientCompositionDisplay;
    clientCompositionDisplay.physicalDisplay = mState.scissor;
    clientCompositionDisplay.clip = mState.scissor;
    clientCompositionDisplay.globalTransform = mState.transform.asMatrix4();
    clientCompositionDisplay.orientation = mState.orientation;
    clientCompositionDisplay.outputDataspace =
            mDisplayColorProfile->hasWideColorGamut() ? mState.dataspace : ui::Dataspace::UNKNOWN;
    clientCompositionDisplay.maxLuminance =
            mDisplayColorProfile->getHdrCapabilities().getDesiredMaxLuminance();

    // Compute the global color transform matrix.
    if (!mState.usesDeviceComposition && !getSkipColorTransform()) {
        clientCompositionDisplay.colorTransform = mState.colorTransformMatrix;
    }

    // Note: Updated by generateClientCompositionRequests
    clientCompositionDisplay.clearRegion = Region::INVALID_REGION;

    // Generate the client composition requests for the layers on this output.
    std::vector<renderengine::LayerSettings> clientCompositionLayers =
            generateClientCompositionRequests(supportsProtectedContent,
                                              clientCompositionDisplay.clearRegion);
    appendRegionFlashRequests(debugRegion, clientCompositionLayers);

    // If we the display is secure, protected content support is enabled, and at
    // least one layer has protected content, we need to use a secure back
    // buffer.
    if (mState.isSecure && supportsProtectedContent) {
        bool needsProtected =
                std::any_of(mOutputLayersOrderedByZ.begin(), mOutputLayersOrderedByZ.end(),
                            [](auto& layer) {
                                return layer->getLayer().getFEState().hasProtectedContent;
                            });
        if (needsProtected != renderEngine.isProtected()) {
            renderEngine.useProtectedContext(needsProtected);
        }
        if (needsProtected != mRenderSurface->isProtected() &&
            needsProtected == renderEngine.isProtected()) {
            mRenderSurface->setProtected(needsProtected);
        }
    }

    base::unique_fd fd;
    sp<GraphicBuffer> buf = mRenderSurface->dequeueBuffer(&fd);
    if (buf == nullptr) {
        ALOGW("Dequeuing buffer for display [%s] failed, bailing out of "
              "client composition for this frame",
              mName.c_str());
        return std::nullopt;
    }

    // We boost GPU frequency here because there will be color spaces conversion
    // and it's expensive. We boost the GPU frequency so that GPU composition can
    // finish in time. We must reset GPU frequency afterwards, because high frequency
    // consumes extra battery.
    const bool expensiveRenderingExpected =
            clientCompositionDisplay.outputDataspace == ui::Dataspace::DISPLAY_P3;
    if (expensiveRenderingExpected) {
        setExpensiveRenderingExpected(true);
    }

    renderEngine.drawLayers(clientCompositionDisplay, clientCompositionLayers,
                            buf->getNativeBuffer(), /*useFramebufferCache=*/true, std::move(fd),
                            &readyFence);

    if (expensiveRenderingExpected) {
        setExpensiveRenderingExpected(false);
    }

    return readyFence;
}

std::vector<renderengine::LayerSettings> Output::generateClientCompositionRequests(
        bool supportsProtectedContent, Region& clearRegion) {
    std::vector<renderengine::LayerSettings> clientCompositionLayers;
    ALOGV("Rendering client layers");

    const Region viewportRegion(mState.viewport);
    const bool useIdentityTransform = false;
    bool firstLayer = true;
    // Used when a layer clears part of the buffer.
    Region dummyRegion;

    for (auto& layer : mOutputLayersOrderedByZ) {
        const auto& layerState = layer->getState();
        const auto& layerFEState = layer->getLayer().getFEState();
        auto& layerFE = layer->getLayerFE();

        const Region clip(viewportRegion.intersect(layerState.visibleRegion));
        ALOGV("Layer: %s", layerFE.getDebugName());
        if (clip.isEmpty()) {
            ALOGV("  Skipping for empty clip");
            firstLayer = false;
            continue;
        }

        bool clientComposition = layer->requiresClientComposition();

        // We clear the client target for non-client composed layers if
        // requested by the HWC. We skip this if the layer is not an opaque
        // rectangle, as by definition the layer must blend with whatever is
        // underneath. We also skip the first layer as the buffer target is
        // guaranteed to start out cleared.
        bool clearClientComposition =
                layerState.clearClientTarget && layerFEState.isOpaque && !firstLayer;

        ALOGV("  Composition type: client %d clear %d", clientComposition, clearClientComposition);

        if (clientComposition || clearClientComposition) {
            compositionengine::LayerFE::ClientCompositionTargetSettings targetSettings{
                    clip,
                    useIdentityTransform,
                    layer->needsFiltering() || mState.needsFiltering,
                    mState.isSecure,
                    supportsProtectedContent,
                    clientComposition ? clearRegion : dummyRegion,
            };
            if (auto result = layerFE.prepareClientComposition(targetSettings)) {
                if (!clientComposition) {
                    auto& layerSettings = *result;
                    layerSettings.source.buffer.buffer = nullptr;
                    layerSettings.source.solidColor = half3(0.0, 0.0, 0.0);
                    layerSettings.alpha = half(0.0);
                    layerSettings.disableBlending = true;
                }

                clientCompositionLayers.push_back(*result);
            }
        }

        firstLayer = false;
    }

    return clientCompositionLayers;
}

void Output::appendRegionFlashRequests(
        const Region& flashRegion,
        std::vector<renderengine::LayerSettings>& clientCompositionLayers) {
    if (flashRegion.isEmpty()) {
        return;
    }

    renderengine::LayerSettings layerSettings;
    layerSettings.source.buffer.buffer = nullptr;
    layerSettings.source.solidColor = half3(1.0, 0.0, 1.0);
    layerSettings.alpha = half(1.0);

    for (const auto& rect : flashRegion) {
        layerSettings.geometry.boundaries = rect.toFloatRect();
        clientCompositionLayers.push_back(layerSettings);
    }
}

void Output::setExpensiveRenderingExpected(bool) {
    // The base class does nothing with this call.
}

void Output::postFramebuffer() {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    if (!getState().isEnabled) {
        return;
    }

    mState.dirtyRegion.clear();
    mRenderSurface->flip();

    auto frame = presentAndGetFrameFences();

    mRenderSurface->onPresentDisplayCompleted();

    for (auto& layer : getOutputLayersOrderedByZ()) {
        // The layer buffer from the previous frame (if any) is released
        // by HWC only when the release fence from this frame (if any) is
        // signaled.  Always get the release fence from HWC first.
        sp<Fence> releaseFence = Fence::NO_FENCE;

        if (auto hwcLayer = layer->getHwcLayer()) {
            if (auto f = frame.layerFences.find(hwcLayer); f != frame.layerFences.end()) {
                releaseFence = f->second;
            }
        }

        // If the layer was client composited in the previous frame, we
        // need to merge with the previous client target acquire fence.
        // Since we do not track that, always merge with the current
        // client target acquire fence when it is available, even though
        // this is suboptimal.
        // TODO(b/121291683): Track previous frame client target acquire fence.
        if (mState.usesClientComposition) {
            releaseFence =
                    Fence::merge("LayerRelease", releaseFence, frame.clientTargetAcquireFence);
        }

        layer->getLayerFE().onLayerDisplayed(releaseFence);
    }

    // We've got a list of layers needing fences, that are disjoint with
    // getOutputLayersOrderedByZ.  The best we can do is to
    // supply them with the present fence.
    for (auto& weakLayer : mReleasedLayers) {
        if (auto layer = weakLayer.promote(); layer != nullptr) {
            layer->onLayerDisplayed(frame.presentFence);
        }
    }

    // Clear out the released layers now that we're done with them.
    mReleasedLayers.clear();
}

void Output::dirtyEntireOutput() {
    mState.dirtyRegion.set(mState.bounds);
}

void Output::chooseCompositionStrategy() {
    // The base output implementation can only do client composition
    mState.usesClientComposition = true;
    mState.usesDeviceComposition = false;
}

bool Output::getSkipColorTransform() const {
    return true;
}

compositionengine::Output::FrameFences Output::presentAndGetFrameFences() {
    compositionengine::Output::FrameFences result;
    if (mState.usesClientComposition) {
        result.clientTargetAcquireFence = mRenderSurface->getClientTargetAcquireFence();
    }
    return result;
}

} // namespace impl
} // namespace android::compositionengine
