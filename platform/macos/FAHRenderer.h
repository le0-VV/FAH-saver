#pragma once

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>

#include <memory>
#include "fah/Model.hpp"

#include "FAHRenderOptions.h"

@interface FAHRenderer : NSObject <MTKViewDelegate>
@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) NSString *lastError;
- (instancetype)initWithDevice:(id<MTLDevice>)device error:(NSError **)error;
- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture;
- (void)setOptions:(const FAHRenderOptions&)options;
- (void)start;
- (void)stop;
- (void)beginMeasurement;
- (id<MTLCommandBuffer>)renderToTexture:(id<MTLTexture>)texture
                                time:(double)time
                            drawable:(id<CAMetalDrawable>)drawable;
- (NSDictionary *)metrics;
@end
