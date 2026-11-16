#pragma once

#import <WebKit/WebKit.h>
#include "FAHRenderOptions.h"
#include <memory>

// Networking and model ownership remain native; this object owns only presentation.
@interface FAHWebRenderer : NSObject
@property(nonatomic, readonly) WKWebView *webView;
@property(nonatomic, readonly) BOOL ready;
@property(nonatomic, readonly) NSString *lastError;
@property(nonatomic, copy) void (^stateChanged)(void);
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL error:(NSError **)error;
- (WKWebView *)prepareWithFrame:(NSRect)frame;
- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture;
- (void)setOptions:(const FAHRenderOptions&)options;
- (void)start;
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
