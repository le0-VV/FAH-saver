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
- (void)beginMeasurement;
- (NSDictionary *)metrics;
- (void)refreshMetrics:(void (^)(NSDictionary *metrics))completion;
- (void)renderAt:(double)seconds completion:(void (^)(NSDictionary *metrics, NSError *error))completion;
@end
