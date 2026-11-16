#pragma once

#import <ScreenSaver/ScreenSaver.h>
#import "FAHRenderer.h"
#include "fah/Source.hpp"
#include <filesystem>
#include <functional>

using FAHSaverSourceFactory = std::function<std::unique_ptr<fah::Source>(fah::SourceOptions, fah::SourceCallback)>;

@interface FAHSaverView : ScreenSaverView
@property(nonatomic, readonly) BOOL hasActiveSource;
@property(nonatomic, readonly) NSUInteger sourceCallbackCount;
@property(nonatomic, readonly) NSUInteger animationCallbackCount;
- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview preferences:(NSUserDefaults *)preferences;
// Test hosts can supply an isolated cache and a source that never opens a socket.
- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview preferences:(NSUserDefaults *)preferences
                    cachePath:(const std::filesystem::path&)cachePath sourceFactory:(FAHSaverSourceFactory)sourceFactory;
- (void)beginMeasurement;
- (NSDictionary *)renderMetrics;
@end
