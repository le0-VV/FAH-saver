#pragma once

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>

#include <memory>
#include "fah/Model.hpp"

enum class FAHStyleMode { Fixed, Cycle, Random };

struct FAHRenderOptions {
    int fps = 60;
    fah::Style style = fah::Style::SpaceFilling;
    FAHStyleMode styleMode = FAHStyleMode::Fixed;
    bool loop = false;
    bool sky = true;
    fah::Vec3 colour = {0.02f, 0.025f, 0.035f};
    double snapshotInterval = 0.1;
    double styleInterval = 20;
    bool collectMetrics = false;
};

@interface FAHRenderer : NSObject <MTKViewDelegate>
@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) NSString *lastError;
- (instancetype)initWithDevice:(id<MTLDevice>)device error:(NSError **)error;
- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture;
- (void)setOptions:(const FAHRenderOptions&)options;
- (void)start;
- (void)stop;
- (id<MTLCommandBuffer>)renderToTexture:(id<MTLTexture>)texture
                                time:(double)time
                            drawable:(id<CAMetalDrawable>)drawable;
- (NSDictionary *)metrics;
@end
