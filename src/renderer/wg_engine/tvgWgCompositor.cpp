/*
 * Copyright (c) 2024 - 2025 the ThorVG project. All rights reserved.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "tvgWgCompositor.h"
#include "tvgWgShaderTypes.h"
#include <iostream>

void WgCompositor::initialize(WgContext& context, uint32_t width, uint32_t height)
{
    // pipelines (external handle, do not release)
    pipelines.initialize(context);
    // initialize opacity pool
    initPools(context);
    // allocate global view matrix handles
    WgShaderTypeMat4x4f viewMat(width, height);
    context.allocateBufferUniform(bufferViewMat, &viewMat, sizeof(viewMat));
    bindGroupViewMat = context.layouts.createBindGroupBuffer1Un(bufferViewMat);
    // create render targets handles
    resize(context, width, height);
    // composition and blend geometries
    meshData.blitBox(context);
}


void WgCompositor::initPools(WgContext& context)
{
    for (uint32_t i = 0; i < 256; i++) {
        float opacity = i / 255.0f;
        context.allocateBufferUniform(bufferOpacities[i], &opacity, sizeof(float));
        bindGroupOpacities[i] = context.layouts.createBindGroupBuffer1Un(bufferOpacities[i]);
    }
}


void WgCompositor::release(WgContext& context)
{
    // composition and blend geometries
    meshData.release(context);
    // release render targets habdles
    resize(context, 0, 0);
    // release opacity pool
    releasePools(context);
    // release global view matrix handles
    context.layouts.releaseBindGroup(bindGroupViewMat);
    context.releaseBuffer(bufferViewMat);
    // release pipelines
    pipelines.release(context);
}


void WgCompositor::releasePools(WgContext& context)
{
    // release opacity pool
    for (uint32_t i = 0; i < 256; i++) {
        context.layouts.releaseBindGroup(bindGroupOpacities[i]);
        context.releaseBuffer(bufferOpacities[i]);
    }
}


void WgCompositor::resize(WgContext& context, uint32_t width, uint32_t height) {
    // release existig handles
    if ((this->width != width) || (this->height != height)) {
        context.layouts.releaseBindGroup(bindGroupStorageTemp);
        // release intermediate render storages
        storageTemp1.release(context);
        storageTemp0.release(context);
        // release global stencil buffer handles
        context.releaseTextureView(texViewDepthStencilMS);
        context.releaseTexture(texDepthStencilMS);
        context.releaseTextureView(texViewDepthStencil);
        context.releaseTexture(texDepthStencil);
        // store render target dimensions
        this->height = height;
        this->width = width;
    }

    // create render targets handles
    if ((width != 0) && (height != 0)) {
        // store render target dimensions
        this->width = width;
        this->height = height;
        // reallocate global view matrix handles
        WgShaderTypeMat4x4f viewMat(width, height);
        context.allocateBufferUniform(bufferViewMat, &viewMat, sizeof(viewMat));
        // allocate global stencil buffer handles
        texDepthStencil = context.createTexAttachement(width, height, WGPUTextureFormat_Depth24PlusStencil8, 1);
        texViewDepthStencil = context.createTextureView(texDepthStencil);
        texDepthStencilMS = context.createTexAttachement(width, height, WGPUTextureFormat_Depth24PlusStencil8, 4);
        texViewDepthStencilMS = context.createTextureView(texDepthStencilMS);
        // initialize intermediate render storages
        storageTemp0.initialize(context, width, height);
        storageTemp1.initialize(context, width, height);
        bindGroupStorageTemp = context.layouts.createBindGroupStrorage2RO(storageTemp0.texView, storageTemp1.texView);
    }
}


RenderRegion WgCompositor::shrinkRenderRegion(RenderRegion& rect)
{
    // cut viewport to screen dimensions
    int32_t xmin = std::max(0, std::min((int32_t)width, rect.x));
    int32_t ymin = std::max(0, std::min((int32_t)height, rect.y));
    int32_t xmax = std::max(xmin, std::min((int32_t)width, rect.x + rect.w));
    int32_t ymax = std::max(ymin, std::min((int32_t)height, rect.y + rect.h));
    return { xmin, ymin, xmax - xmin, ymax - ymin };
}


void WgCompositor::copyTexture(const WgRenderStorage* dst, const WgRenderStorage* src)
{
    const RenderRegion region = { 0, 0, (int32_t)src->width, (int32_t)src->height };
    copyTexture(dst, src, region);
}


void WgCompositor::copyTexture(const WgRenderStorage* dst, const WgRenderStorage* src, const RenderRegion& region)
{
    assert(dst);
    assert(src);
    assert(commandEncoder);
    const WGPUImageCopyTexture texSrc { .texture = src->texture, .origin = { .x = (uint32_t)region.x, .y = (uint32_t)region.y } };
    const WGPUImageCopyTexture texDst { .texture = dst->texture, .origin = { .x = (uint32_t)region.x, .y = (uint32_t)region.y } };
    const WGPUExtent3D copySize { .width = (uint32_t)region.w, .height = (uint32_t)region.h, .depthOrArrayLayers = 1 };
    wgpuCommandEncoderCopyTextureToTexture(commandEncoder, &texSrc, &texDst, &copySize);
}


void WgCompositor::beginRenderPass(WGPUCommandEncoder commandEncoder, WgRenderStorage* target, bool clear, WGPUColor clearColor)
{
    assert(commandEncoder);
    assert(target);
    this->currentTarget = target;
    this->commandEncoder = commandEncoder;
    const WGPURenderPassDepthStencilAttachment depthStencilAttachment{ 
        .view = texViewDepthStencilMS,
        .depthLoadOp = WGPULoadOp_Clear, .depthStoreOp = WGPUStoreOp_Discard, .depthClearValue = 1.0f,
        .stencilLoadOp = WGPULoadOp_Clear, .stencilStoreOp = WGPUStoreOp_Discard, .stencilClearValue = 0
    };
    const WGPURenderPassColorAttachment colorAttachment{
        .view = target->texViewMS,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .resolveTarget = target->texView,
        .loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = clearColor
        
    };
    WGPURenderPassDescriptor renderPassDesc{ .colorAttachmentCount = 1, .colorAttachments = &colorAttachment, .depthStencilAttachment = &depthStencilAttachment };
    renderPassEncoder = wgpuCommandEncoderBeginRenderPass(commandEncoder, &renderPassDesc);
    assert(renderPassEncoder);
}


void WgCompositor::endRenderPass()
{
    if (currentTarget) {
        assert(renderPassEncoder);
        wgpuRenderPassEncoderEnd(renderPassEncoder);
        wgpuRenderPassEncoderRelease(renderPassEncoder);
        this->renderPassEncoder = nullptr;
        this->currentTarget = nullptr;
    }
}


void WgCompositor::renderShape(WgContext& context, WgRenderDataShape* renderData, BlendMethod blendMethod)
{
    assert(renderData);
    assert(renderPassEncoder);
    // apply clip path if neccessary
    if (renderData->clips.count != 0) {
        renderClipPath(context, renderData);
        if (renderData->strokeFirst) {
            clipStrokes(context, renderData);
            clipShape(context, renderData);
        } else {
            clipShape(context, renderData);
            clipStrokes(context, renderData);
        }
        clearClipPath(context, renderData);
    // use custom blending
    } else if (blendMethod != BlendMethod::Normal) {
        if (renderData->strokeFirst) {
            blendStrokes(context, renderData, blendMethod);
            blendShape(context, renderData, blendMethod);
        } else {
            blendShape(context, renderData, blendMethod);
            blendStrokes(context, renderData, blendMethod);
        }
    // use direct hardware blending
    } else {
        if (renderData->strokeFirst) {
            drawStrokes(context, renderData);
            drawShape(context, renderData);
        } else {
            drawShape(context, renderData);
            drawStrokes(context, renderData);
        }
    }
}


void WgCompositor::renderImage(WgContext& context, WgRenderDataPicture* renderData, BlendMethod blendMethod)
{
    assert(renderData);
    assert(renderPassEncoder);
    // apply clip path if neccessary
    if (renderData->clips.count != 0) {
        renderClipPath(context, renderData);
        clipImage(context, renderData);
        clearClipPath(context, renderData);
    // use custom blending
    } else if (blendMethod != BlendMethod::Normal)
        blendImage(context, renderData, blendMethod);
    // use direct hardware blending
    else drawImage(context, renderData);
}


void WgCompositor::renderScene(WgContext& context, WgRenderStorage* scene, WgCompose* compose)
{
    assert(scene);
    assert(compose);
    assert(renderPassEncoder);
    // use custom blending
    if (compose->blend != BlendMethod::Normal)
        blendScene(context, scene, compose);
    // use direct hardware blending
    else drawScene(context, scene, compose);
}


void WgCompositor::composeScene(WgContext& context, WgRenderStorage* src, WgRenderStorage* mask, WgCompose* cmp)
{
    assert(cmp);
    assert(src);
    assert(mask);
    assert(renderPassEncoder);
    RenderRegion rect = shrinkRenderRegion(cmp->aabb);
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, rect.x, rect.y, rect.w, rect.h);
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, src->bindGroupTexure, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, mask->bindGroupTexure, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.scene_compose[(uint32_t)cmp->method]);
    meshData.drawImage(context, renderPassEncoder);
}


void WgCompositor::blit(WgContext& context, WGPUCommandEncoder encoder, WgRenderStorage* src, WGPUTextureView dstView) {
    const WGPURenderPassDepthStencilAttachment depthStencilAttachment{ 
        .view = texViewDepthStencil,
        .depthLoadOp = WGPULoadOp_Load, .depthStoreOp = WGPUStoreOp_Discard,
        .stencilLoadOp = WGPULoadOp_Load, .stencilStoreOp = WGPUStoreOp_Discard
    };
    const WGPURenderPassColorAttachment colorAttachment { 
        .view = dstView,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Load,
        .storeOp = WGPUStoreOp_Store,
    };
    const WGPURenderPassDescriptor renderPassDesc{ .colorAttachmentCount = 1, .colorAttachments = &colorAttachment, .depthStencilAttachment = &depthStencilAttachment };
    WGPURenderPassEncoder renderPass = wgpuCommandEncoderBeginRenderPass(encoder, &renderPassDesc);
    wgpuRenderPassEncoderSetBindGroup(renderPass, 0, src->bindGroupTexure, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPass, pipelines.blit);
    meshData.drawImage(context, renderPass);
    wgpuRenderPassEncoderEnd(renderPass);
    wgpuRenderPassEncoderRelease(renderPass);
}


void WgCompositor::drawShape(WgContext& context, WgRenderDataShape* renderData)
{
    assert(renderData);
    assert(renderPassEncoder);
    assert(renderData->meshGroupShapes.meshes.count == renderData->meshGroupShapesBBox.meshes.count);
    if (renderData->renderSettingsShape.skip) return;
    if (renderData->meshGroupShapes.meshes.count == 0) return;
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // setup stencil rules
    WGPURenderPipeline stencilPipeline = (renderData->fillRule == FillRule::NonZero) ? pipelines.nonzero : pipelines.evenodd;
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, stencilPipeline);
    // draw to stencil (first pass)
    ARRAY_FOREACH(p, renderData->meshGroupShapes.meshes)
        (*p)->drawFan(context, renderPassEncoder);
    // setup fill rules
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    WgRenderSettings& settings = renderData->renderSettingsShape;
    if (settings.fillType == WgRenderSettingsType::Solid) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupSolid, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.solid);
    } else if (settings.fillType == WgRenderSettingsType::Linear) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.linear);
    } else if (settings.fillType == WgRenderSettingsType::Radial) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.radial);
    }
    // draw to color (second pass)
    renderData->meshDataBBox.drawFan(context, renderPassEncoder);
}


void WgCompositor::blendShape(WgContext& context, WgRenderDataShape* renderData, BlendMethod blendMethod)
{
    assert(renderData);
    assert(renderPassEncoder);
    assert(renderData->meshGroupShapes.meshes.count == renderData->meshGroupShapesBBox.meshes.count);
    if (renderData->renderSettingsShape.skip) return;
    if (renderData->meshGroupShapes.meshes.count == 0) return;
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    // copy current render target data to dst storage
    WgRenderStorage *target = currentTarget;
    endRenderPass();
    copyTexture(&storageTemp0, target);
    beginRenderPass(commandEncoder, target, false);
    // render shape with blend settings
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // setup stencil rules
    WGPURenderPipeline stencilPipeline = (renderData->fillRule == FillRule::NonZero) ? pipelines.nonzero : pipelines.evenodd;
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, stencilPipeline);
    // draw to stencil (first pass)
    ARRAY_FOREACH(p, renderData->meshGroupShapes.meshes)
        (*p)->drawFan(context, renderPassEncoder);
    // setup fill rules
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 3, storageTemp0.bindGroupTexure, 0, nullptr);
    uint32_t blendMethodInd = (uint32_t)blendMethod;
    WgRenderSettings& settings = renderData->renderSettingsShape;
    if (settings.fillType == WgRenderSettingsType::Solid) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupSolid, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.solid_blend[blendMethodInd]);
    } else if (settings.fillType == WgRenderSettingsType::Linear) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.linear_blend[blendMethodInd]);
    } else if (settings.fillType == WgRenderSettingsType::Radial) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.radial_blend[blendMethodInd]);
    }
    // draw to color (second pass)
    renderData->meshDataBBox.drawFan(context, renderPassEncoder);
}


void WgCompositor::clipShape(WgContext& context, WgRenderDataShape* renderData)
{
    assert(renderData);
    assert(renderPassEncoder);
    assert(renderData->meshGroupShapes.meshes.count == renderData->meshGroupShapesBBox.meshes.count);
    if (renderData->renderSettingsShape.skip) return;
    if (renderData->meshGroupShapes.meshes.count == 0) return;
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // setup stencil rules
    WGPURenderPipeline stencilPipeline = (renderData->fillRule == FillRule::NonZero) ? pipelines.nonzero : pipelines.evenodd;
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, stencilPipeline);
    // draw to stencil (first pass)
    ARRAY_FOREACH(p, renderData->meshGroupShapes.meshes)
        (*p)->drawFan(context, renderPassEncoder);
    // merge depth and stencil buffer
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[128], 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.merge_depth_stencil);
    renderData->meshDataBBox.drawFan(context, renderPassEncoder);
    // setup fill rules
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    WgRenderSettings& settings = renderData->renderSettingsShape;
    if (settings.fillType == WgRenderSettingsType::Solid) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupSolid, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.solid);
    } else if (settings.fillType == WgRenderSettingsType::Linear) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.linear);
    } else if (settings.fillType == WgRenderSettingsType::Radial) {
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.radial);
    }
    // draw to color (second pass)
    renderData->meshDataBBox.drawFan(context, renderPassEncoder);
}


void WgCompositor::drawStrokes(WgContext& context, WgRenderDataShape* renderData)
{
    assert(renderData);
    assert(renderPassEncoder);
    assert(renderData->meshGroupStrokes.meshes.count == renderData->meshGroupStrokesBBox.meshes.count);
    if (renderData->renderSettingsStroke.skip) return;
    if (renderData->meshGroupStrokes.meshes.count == 0) return;
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // draw strokes to stencil (first pass)
    for (uint32_t i = 0; i < renderData->meshGroupStrokes.meshes.count; i++) {
        // setup stencil rules
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 255);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.direct);
        // draw to stencil (first pass)
        renderData->meshGroupStrokes.meshes[i]->draw(context, renderPassEncoder);
        // setup fill rules
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        WgRenderSettings& settings = renderData->renderSettingsStroke;
        if (settings.fillType == WgRenderSettingsType::Solid) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupSolid, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.solid);
        } else if (settings.fillType == WgRenderSettingsType::Linear) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.linear);
        } else if (settings.fillType == WgRenderSettingsType::Radial) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.radial);
        }
        // draw to color (second pass)
        renderData->meshGroupStrokesBBox.meshes[i]->drawFan(context, renderPassEncoder);
    }
}


void WgCompositor::blendStrokes(WgContext& context, WgRenderDataShape* renderData, BlendMethod blendMethod)
{
    assert(renderData);
    assert(renderPassEncoder);
    assert(renderData->meshGroupStrokes.meshes.count == renderData->meshGroupStrokesBBox.meshes.count);
    if (renderData->renderSettingsStroke.skip) return;
    if (renderData->meshGroupStrokes.meshes.count == 0) return;
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    // copy current render target data to dst storage
    WgRenderStorage *target = currentTarget;
    endRenderPass();
    copyTexture(&storageTemp0, target);
    beginRenderPass(commandEncoder, target, false);
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // draw strokes to stencil (first pass)
    for (uint32_t i = 0; i < renderData->meshGroupStrokes.meshes.count; i++) {
        // setup stencil rules
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 255);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.direct);
        // draw to stencil (first pass)
        renderData->meshGroupStrokes.meshes[i]->draw(context, renderPassEncoder);
        // setup fill rules
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 3, storageTemp0.bindGroupTexure, 0, nullptr);
        uint32_t blendMethodInd = (uint32_t)blendMethod;
        WgRenderSettings& settings = renderData->renderSettingsStroke;
        if (settings.fillType == WgRenderSettingsType::Solid) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupSolid, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.solid_blend[blendMethodInd]);
        } else if (settings.fillType == WgRenderSettingsType::Linear) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.linear_blend[blendMethodInd]);
        } else if (settings.fillType == WgRenderSettingsType::Radial) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.radial_blend[blendMethodInd]);
        }
        // draw to color (second pass)
        renderData->meshGroupStrokesBBox.meshes[i]->drawFan(context, renderPassEncoder);
    }
};


void WgCompositor::clipStrokes(WgContext& context, WgRenderDataShape* renderData)
{
    assert(renderData);
    assert(renderPassEncoder);
    assert(renderData->meshGroupStrokes.meshes.count == renderData->meshGroupStrokesBBox.meshes.count);
    if (renderData->renderSettingsStroke.skip) return;
    if (renderData->meshGroupStrokes.meshes.count == 0) return;
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // draw strokes to stencil (first pass)
    for (uint32_t i = 0; i < renderData->meshGroupStrokes.meshes.count; i++) {
        // setup stencil rules
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 255);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.direct);
        // draw to stencil (first pass)
        renderData->meshGroupStrokes.meshes[i]->draw(context, renderPassEncoder);
        // merge depth and stencil buffer
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[128], 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.merge_depth_stencil);
        renderData->meshDataBBox.drawFan(context, renderPassEncoder);
        // setup fill rules
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        WgRenderSettings& settings = renderData->renderSettingsStroke;
        if (settings.fillType == WgRenderSettingsType::Solid) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupSolid, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.solid);
        } else if (settings.fillType == WgRenderSettingsType::Linear) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.linear);
        } else if (settings.fillType == WgRenderSettingsType::Radial) {
            wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, settings.bindGroupGradient, 0, nullptr);
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.radial);
        }
        // draw to color (second pass)
        renderData->meshGroupStrokesBBox.meshes[i]->drawFan(context, renderPassEncoder);
    }
}


void WgCompositor::drawImage(WgContext& context, WgRenderDataPicture* renderData)
{
    assert(renderData);
    assert(renderPassEncoder);
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // draw stencil
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 255);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.direct);
    renderData->meshData.drawImage(context, renderPassEncoder);
    // draw image
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, renderData->bindGroupPicture, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.image);
    renderData->meshData.drawImage(context, renderPassEncoder);
}


void WgCompositor::blendImage(WgContext& context, WgRenderDataPicture* renderData, BlendMethod blendMethod)
{
    assert(renderData);
    assert(renderPassEncoder);
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // copy current render target data to dst storage
    WgRenderStorage *target = currentTarget;
    endRenderPass();
    copyTexture(&storageTemp0, target);
    beginRenderPass(commandEncoder, target, false);
    // setup stencil rules
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 255);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.direct);
    renderData->meshData.drawImage(context, renderPassEncoder);
    // blend image
    uint32_t blendMethodInd = (uint32_t)blendMethod;
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, renderData->bindGroupPicture, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 3, storageTemp0.bindGroupTexure, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.image_blend[blendMethodInd]);
    renderData->meshData.drawImage(context, renderPassEncoder);
};


void WgCompositor::clipImage(WgContext& context, WgRenderDataPicture* renderData)
{
    assert(renderData);
    assert(renderPassEncoder);
    if ((renderData->viewport.w <= 0) || (renderData->viewport.h <= 0)) return;
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, renderData->viewport.x, renderData->viewport.y, renderData->viewport.w, renderData->viewport.h);
    // setup stencil rules
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 255);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.direct);
    renderData->meshData.drawImage(context, renderPassEncoder);
    // merge depth and stencil buffer
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[128], 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.merge_depth_stencil);
    renderData->meshData.drawImage(context, renderPassEncoder);
    // draw image
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, renderData->bindGroupPicture, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.image);
    renderData->meshData.drawImage(context, renderPassEncoder);
}


void WgCompositor::drawScene(WgContext& context, WgRenderStorage* scene, WgCompose* compose)
{
    assert(scene);
    assert(compose);
    assert(currentTarget);
    // draw scene
    RenderRegion rect = shrinkRenderRegion(compose->aabb);
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, rect.x, rect.y, rect.w, rect.h);
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, scene->bindGroupTexure, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, bindGroupOpacities[compose->opacity], 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.scene);
    meshData.drawImage(context, renderPassEncoder);
}


void WgCompositor::blendScene(WgContext& context, WgRenderStorage* scene, WgCompose* compose)
{
    assert(scene);
    assert(compose);
    assert(currentTarget);
    // copy current render target data to dst storage
    WgRenderStorage *target = currentTarget;
    endRenderPass();
    copyTexture(&storageTemp0, target);
    beginRenderPass(commandEncoder, target, false);
    // blend scene
    uint32_t blendMethodInd = (uint32_t)compose->blend;
    RenderRegion rect = shrinkRenderRegion(compose->aabb);
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, rect.x, rect.y, rect.w, rect.h);
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, scene->bindGroupTexure, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, storageTemp0.bindGroupTexure, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[compose->opacity], 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.scene_blend[blendMethodInd]);
    meshData.drawImage(context, renderPassEncoder);
}


void WgCompositor::markupClipPath(WgContext& context, WgRenderDataShape* renderData)
{
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
    // markup stencil
    if (renderData->meshGroupStrokes.meshes.count > 0) {
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 255);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.direct);
        ARRAY_FOREACH(p, renderData->meshGroupStrokes.meshes)
            (*p)->draw(context, renderPassEncoder);
    } else {
        WGPURenderPipeline stencilPipeline = (renderData->fillRule == FillRule::NonZero) ? pipelines.nonzero : pipelines.evenodd;
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, stencilPipeline);
        ARRAY_FOREACH(p, renderData->meshGroupShapes.meshes)
            (*p)->drawFan(context, renderPassEncoder);
    }
}


void WgCompositor::renderClipPath(WgContext& context, WgRenderDataPaint* paint)
{
    assert(paint);
    assert(renderPassEncoder);
    assert(paint->clips.count > 0);
    // reset scissor recr to full screen
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, 0, 0, width, height);
    // get render data
    WgRenderDataShape* renderData0 = (WgRenderDataShape*)paint->clips[0];
    // markup stencil
    markupClipPath(context, renderData0);
    // copy stencil to depth
    wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData0->bindGroupPaint, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[128], 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.copy_stencil_to_depth);
    renderData0->meshDataBBox.drawFan(context, renderPassEncoder);
    // merge clip pathes with AND logic
    for (auto p = paint->clips.begin() + 1; p < paint->clips.end(); ++p) {
        // get render data
        WgRenderDataShape* renderData = (WgRenderDataShape*)(*p);
        // markup stencil
        markupClipPath(context, renderData);
        // copy stencil to depth (clear stencil)
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[190], 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.copy_stencil_to_depth_interm);
        renderData->meshDataBBox.drawFan(context, renderPassEncoder);
        // copy depth to stencil
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 1);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[190], 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.copy_depth_to_stencil);
        renderData->meshDataBBox.drawFan(context, renderPassEncoder);
        // clear depth current (keep stencil)
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[255], 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.clear_depth);
        renderData->meshDataBBox.drawFan(context, renderPassEncoder);
        // clear depth original (keep stencil)
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData0->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[255], 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.clear_depth);
        renderData0->meshDataBBox.drawFan(context, renderPassEncoder);
        // copy stencil to depth (clear stencil)
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[128], 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.copy_stencil_to_depth);
        renderData->meshDataBBox.drawFan(context, renderPassEncoder);
    }
}


void WgCompositor::clearClipPath(WgContext& context, WgRenderDataPaint* paint)
{
    assert(paint);
    assert(renderPassEncoder);
    assert(paint->clips.count > 0);
    // reset scissor recr to full screen
    wgpuRenderPassEncoderSetScissorRect(renderPassEncoder, 0, 0, width, height);
    // get render data
    ARRAY_FOREACH(p, paint->clips) {
        WgRenderDataShape* renderData = (WgRenderDataShape*)(*p);
        // set transformations
        wgpuRenderPassEncoderSetStencilReference(renderPassEncoder, 0);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 0, bindGroupViewMat, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 1, renderData->bindGroupPaint, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(renderPassEncoder, 2, bindGroupOpacities[255], 0, nullptr);
        wgpuRenderPassEncoderSetPipeline(renderPassEncoder, pipelines.clear_depth);
        renderData->meshDataBBox.drawFan(context, renderPassEncoder);
    }
}


bool WgCompositor::gaussianBlur(WgContext& context, WgRenderStorage* dst, const RenderEffectGaussianBlur* params, const WgCompose* compose)
{
    assert(dst);
    assert(params);
    assert(params->rd);
    assert(compose->rdViewport);
    assert(!renderPassEncoder);
    auto renderData = (WgRenderDataEffectParams*)params->rd;
    auto aabb = compose->aabb;
    auto viewport = compose->rdViewport;
    WgRenderStorage* sbuff = dst;
    WgRenderStorage* dbuff = &storageTemp0;

    // begin compute pass
    WGPUComputePassDescriptor computePassDesc{ .label = "Compute pass gaussian blur" };
    WGPUComputePassEncoder computePassEncoder = wgpuCommandEncoderBeginComputePass(commandEncoder, &computePassDesc);
    for (uint32_t level = 0; level < renderData->level; level++) {
        // horizontal blur
        if (params->direction != 2) {
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, sbuff->bindGroupRead, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dbuff->bindGroupWrite, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, renderData->bindGroupParams, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, viewport->bindGroupViewport, 0, nullptr);
            wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.gaussian_horz);
            wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, (aabb.w - 1) / 128 + 1, aabb.h, 1);
            std::swap(sbuff, dbuff);
        }
        // vertical blur
        if (params->direction != 1) {
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, sbuff->bindGroupRead, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dbuff->bindGroupWrite, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, renderData->bindGroupParams, 0, nullptr);
            wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, viewport->bindGroupViewport, 0, nullptr);
            wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.gaussian_vert);
            wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, aabb.w, (aabb.h - 1) / 128 + 1, 1);
            std::swap(sbuff, dbuff);
        }
    }
    // end compute pass
    wgpuComputePassEncoderEnd(computePassEncoder);
    wgpuComputePassEncoderRelease(computePassEncoder);

    // if final result stored in intermidiate buffer we must copy result to destination buffer
    if (sbuff == &storageTemp0) 
        copyTexture(sbuff, dbuff, aabb);

    return true;
}


bool WgCompositor::dropShadow(WgContext& context, WgRenderStorage* dst, const RenderEffectDropShadow* params, const WgCompose* compose)
{
    assert(dst);
    assert(params);
    assert(params->rd);
    assert(compose->rdViewport);
    assert(!renderPassEncoder);

    auto renderDataParams = (WgRenderDataEffectParams*)params->rd;
    auto aabb = compose->aabb;
    auto viewport = compose->rdViewport;

    { // apply blur
        copyTexture(&storageTemp1, dst, aabb);
        WgRenderStorage* sbuff = &storageTemp1;
        WgRenderStorage* dbuff = &storageTemp0;
        WGPUComputePassDescriptor computePassDesc{ .label = "Compute pass drop shadow blur" };
        WGPUComputePassEncoder computePassEncoder = wgpuCommandEncoderBeginComputePass(commandEncoder, &computePassDesc);
        // horizontal blur
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, sbuff->bindGroupRead, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dbuff->bindGroupWrite, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, renderDataParams->bindGroupParams, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, viewport->bindGroupViewport, 0, nullptr);
        wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.gaussian_horz);
        wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, (aabb.w - 1) / 128 + 1, aabb.h, 1);
        std::swap(sbuff, dbuff);
        // vertical blur
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, sbuff->bindGroupRead, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dbuff->bindGroupWrite, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, renderDataParams->bindGroupParams, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, viewport->bindGroupViewport, 0, nullptr);
        wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.gaussian_vert);
        wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, aabb.w, (aabb.h - 1) / 128 + 1, 1);
        std::swap(sbuff, dbuff);
        wgpuComputePassEncoderEnd(computePassEncoder);
        wgpuComputePassEncoderRelease(computePassEncoder);
    }
    
    { // blend origin (temp0), shadow (temp1) to destination
        copyTexture(&storageTemp0, dst, aabb);
        WGPUComputePassDescriptor computePassDesc{ .label = "Compute pass drop shadow blend" };
        WGPUComputePassEncoder computePassEncoder = wgpuCommandEncoderBeginComputePass(commandEncoder, &computePassDesc);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, bindGroupStorageTemp, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dst->bindGroupWrite, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, renderDataParams->bindGroupParams, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, viewport->bindGroupViewport, 0, nullptr);
        wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.dropshadow);
        wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, (aabb.w - 1) / 128 + 1, aabb.h, 1);
        wgpuComputePassEncoderEnd(computePassEncoder);
        wgpuComputePassEncoderRelease(computePassEncoder);
    }

    return true;
}


bool WgCompositor::fillEffect(WgContext& context, WgRenderStorage* dst, const RenderEffectFill* params, const WgCompose* compose)
{
    assert(dst);
    assert(params);
    assert(params->rd);
    assert(compose->rdViewport);
    assert(!renderPassEncoder);

    copyTexture(&storageTemp0, dst, compose->aabb);
    WGPUComputePassDescriptor computePassDesc{ .label = "Compute pass fill" };
    WGPUComputePassEncoder computePassEncoder = wgpuCommandEncoderBeginComputePass(commandEncoder, &computePassDesc);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, bindGroupStorageTemp, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dst->bindGroupWrite, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, static_cast<WgRenderDataEffectParams*>(params->rd)->bindGroupParams, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, compose->rdViewport->bindGroupViewport, 0, nullptr);
    wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.fill_effect);
    wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, (compose->aabb.w - 1) / 128 + 1, compose->aabb.h, 1);
    wgpuComputePassEncoderEnd(computePassEncoder);
    wgpuComputePassEncoderRelease(computePassEncoder);

    return true;
}


bool WgCompositor::tintEffect(WgContext& context, WgRenderStorage* dst, const RenderEffectTint* params, const WgCompose* compose)
{
    assert(dst);
    assert(params);
    assert(params->rd);
    assert(compose->rdViewport);
    assert(!renderPassEncoder);

    copyTexture(&storageTemp0, dst, compose->aabb);
    WGPUComputePassDescriptor computePassDesc{ .label = "Compute pass tint" };
    WGPUComputePassEncoder computePassEncoder = wgpuCommandEncoderBeginComputePass(commandEncoder, &computePassDesc);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, bindGroupStorageTemp, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dst->bindGroupWrite, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, static_cast<WgRenderDataEffectParams*>(params->rd)->bindGroupParams, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, compose->rdViewport->bindGroupViewport, 0, nullptr);
    wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.tint_effect);
    wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, (compose->aabb.w - 1) / 128 + 1, compose->aabb.h, 1);
    wgpuComputePassEncoderEnd(computePassEncoder);
    wgpuComputePassEncoderRelease(computePassEncoder);

    return true;
}

bool WgCompositor::tritoneEffect(WgContext& context, WgRenderStorage* dst, const RenderEffectTritone* params, const WgCompose* compose)
{
    assert(dst);
    assert(params);
    assert(params->rd);
    assert(compose->rdViewport);
    assert(!renderPassEncoder);

    copyTexture(&storageTemp0, dst, compose->aabb);
    WGPUComputePassDescriptor computePassDesc{ .label = "Compute pass tritone" };
    WGPUComputePassEncoder computePassEncoder = wgpuCommandEncoderBeginComputePass(commandEncoder, &computePassDesc);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 0, bindGroupStorageTemp, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 1, dst->bindGroupWrite, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 2, static_cast<WgRenderDataEffectParams*>(params->rd)->bindGroupParams, 0, nullptr);
    wgpuComputePassEncoderSetBindGroup(computePassEncoder, 3, compose->rdViewport->bindGroupViewport, 0, nullptr);
    wgpuComputePassEncoderSetPipeline(computePassEncoder, pipelines.tritone_effect);
    wgpuComputePassEncoderDispatchWorkgroups(computePassEncoder, (compose->aabb.w - 1) / 128 + 1, compose->aabb.h, 1);
    wgpuComputePassEncoderEnd(computePassEncoder);
    wgpuComputePassEncoderRelease(computePassEncoder);

    return true;
}