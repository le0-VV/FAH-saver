#pragma once

#import <WebKit/WebKit.h>
#include "FAHRenderOptions.h"
#include <memory>

enum class FAHWebBackend { WK, Legacy };
enum class FAHWebClock { AnimationFrame, Native };

// Networking and model ownership remain native; this object owns only presentation.
@interface FAHWebRenderer : NSObject
@property(nonatomic, readonly) NSView *presentationView;
@property(nonatomic, readonly) FAHWebClock clock;
// WK-specific access, nil for the explicit Legacy backend.
@property(nonatomic, readonly) WKWebView *webView;
@property(nonatomic, readonly) BOOL ready;
@property(nonatomic, readonly) NSString *lastError;
@property(nonatomic, copy) void (^stateChanged)(void);
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL error:(NSError **)error;
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL backend:(FAHWebBackend)backend error:(NSError **)error;
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL backend:(FAHWebBackend)backend
                          clock:(FAHWebClock)clock error:(NSError **)error;
- (NSView *)prepareWithFrame:(NSRect)frame;
- (void)evaluateRuntimeScript:(NSString *)script completion:(void (^)(id result, NSError *error))completion;
- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture;
- (void)setOptions:(const FAHRenderOptions&)options;
- (void)start;
// Native clock: accepts the latest elapsed time and coalesces queued JavaScript work.
- (BOOL)advanceTo:(double)seconds;
// Disposes the JavaScript renderer and releases the WebView, even while this owner is retained.
- (void)stop;
// Requests made while the page loads are applied once it is ready; stop cancels them.
- (void)beginMeasurement;
// Returns the last bridge snapshot; refreshMetrics requests a new snapshot from the page.
- (NSDictionary *)metrics;
- (void)refreshMetrics:(void (^)(NSDictionary *metrics))completion;
// A stopped or replaced page completes with a cancellation error.
- (void)renderAt:(double)seconds completion:(void (^)(NSDictionary *metrics, NSError *error))completion;
@end
