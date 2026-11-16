#import "FAHWebRenderer.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <string>

static void configureSharedSession(WKWebViewConfiguration *configuration) {
    // All calls are on WebKit's main thread; the last WebView releases this private session.
    static __weak WKWebsiteDataStore *sharedStore;
    WKWebsiteDataStore *store = sharedStore;
    if (!store) sharedStore = store = WKWebsiteDataStore.nonPersistentDataStore;
    configuration.websiteDataStore = store;
    if (@available(macOS 12.0, *)) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // Process pools affect only the supported macOS 11 path.
    static __weak WKProcessPool *sharedPool;
    WKProcessPool *pool = sharedPool;
    if (!pool) sharedPool = pool = [WKProcessPool new];
    configuration.processPool = pool;
#pragma clang diagnostic pop
}

@interface FAHWeakScriptHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, weak) id<WKScriptMessageHandler> target;
@end
@implementation FAHWeakScriptHandler
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    [self.target userContentController:controller didReceiveScriptMessage:message];
}
@end

@interface FAHWebRenderer () <WKScriptMessageHandler, WKNavigationDelegate>
@end
@implementation FAHWebRenderer {
    NSURL *_assetsURL, *_pageURL;
    WKWebView *_webView;
    FAHWeakScriptHandler *_handler;
    FAHRenderOptions _options;
    std::shared_ptr<const fah::Capture> _capture;
    std::weak_ptr<const fah::Capture> _sentCapture;
    const fah::Capture *_sentIdentity;
    std::size_t _sentFrames;
    NSUInteger _captureRevision, _sentRevision, _generation;
    BOOL _ready, _running, _started, _busy, _optionsDirty, _measuring, _measurementPending;
    NSString *_lastError;
    NSDictionary *_javascriptMetrics;
}
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    _assetsURL = assetsURL.URLByStandardizingPath;
    _pageURL = [_assetsURL URLByAppendingPathComponent:@"saver.html"];
    if (!_assetsURL.isFileURL || ![NSFileManager.defaultManager fileExistsAtPath:_pageURL.path]) {
        if (error) *error = [NSError errorWithDomain:@"org.fahsaver.web-renderer" code:1
            userInfo:@{NSLocalizedDescriptionKey:@"The bundled Web renderer is missing."}];
        return nil;
    }
    return self;
}
- (WKWebView *)webView { return _webView; }
- (BOOL)ready { return _ready; }
- (NSString *)lastError { return _lastError; }
- (void)fail:(NSString *)message {
    _lastError = message.length ? message : @"The Web renderer failed.";
    if (self.stateChanged) self.stateChanged();
}
- (WKWebView *)prepareWithFrame:(NSRect)frame {
    if (_webView) return _webView;
    ++_generation;
    _ready = _busy = _started = NO;
    _lastError = nil;
    _sentIdentity = nullptr; _sentFrames = _sentRevision = 0;
    _optionsDirty = YES;
    WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    configureSharedSession(configuration);
    _handler = [FAHWeakScriptHandler new]; _handler.target = self;
    [configuration.userContentController addScriptMessageHandler:_handler name:@"renderer"];
    _webView = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
    _webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _webView.navigationDelegate = self;
    _webView.allowsBackForwardNavigationGestures = NO;
    [_webView loadFileURL:_pageURL allowingReadAccessToURL:_assetsURL];
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        FAHWebRenderer *owner = weakSelf;
        if (owner && owner->_generation == generation && owner->_webView && !owner->_ready)
            [owner fail:@"The Web renderer did not become ready."];
    });
    return _webView;
}
- (void)setOptions:(const FAHRenderOptions&)options {
    if (options.fps < 1 || options.fps > 240 || !std::isfinite(options.snapshotInterval) || options.snapshotInterval < .05 ||
        !std::isfinite(options.styleInterval) || options.styleInterval <= 0 ||
        static_cast<int>(options.style) < 1 || static_cast<int>(options.style) > 3)
        throw std::runtime_error("Invalid render timing or model style settings");
    for (float value : {options.colour.x, options.colour.y, options.colour.z})
        if (!std::isfinite(value) || value < 0 || value > 1)
            throw std::runtime_error("Background colour components must be between zero and one");
    if (_options.loop != options.loop) { _sentIdentity = nullptr; _sentFrames = 0; }
    _options = options; _optionsDirty = YES;
    [self pump];
}
- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture {
    _capture = std::move(capture); ++_captureRevision;
    [self pump];
}
- (void)start { _running = YES; [self pump]; }
- (void)stop {
    _running = _ready = _started = _busy = _measuring = _measurementPending = NO; ++_generation;
    if (_webView) {
        // Releasing the page also terminates its rAF/context if script execution is interrupted.
        [_webView evaluateJavaScript:@"window.fahSaver && window.fahSaver.dispose()" completionHandler:nil];
        [_webView.configuration.userContentController removeScriptMessageHandlerForName:@"renderer"];
        [_webView.configuration.userContentController removeAllUserScripts];
        _webView.navigationDelegate = nil;
        [_webView stopLoading];
        [_webView removeFromSuperview];
        _webView = nil;
    }
    _handler.target = nil; _handler = nil;
    _sentIdentity = nullptr; _sentFrames = 0; _sentCapture.reset();
}
- (NSString *)configurationScript {
    NSDictionary *value = @{ @"fps":@(_options.fps), @"style":@(static_cast<int>(_options.style)),
        @"styleMode":_options.styleMode == FAHStyleMode::Cycle ? @"cycle" : _options.styleMode == FAHStyleMode::Random ? @"random" : @"fixed",
        @"playback":_options.loop ? @"loop" : @"latest", @"background":!_options.sky ? @"solid" : _options.darkSky ? @"dark" : @"sky",
        @"colour":@[@(_options.colour.x),@(_options.colour.y),@(_options.colour.z)],
        @"snapshotInterval":@(_options.snapshotInterval), @"styleInterval":@(_options.styleInterval),
        @"collectMetrics":@(_options.collectMetrics) };
    NSData *json = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    return [NSString stringWithFormat:@"window.fahSaver.configure(%@);",[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding]];
}
- (void)pump {
    if (!_ready || !_webView || _busy || _lastError) return;
    try {
        NSMutableString *script = [NSMutableString new];
        if (_optionsDirty) { [script appendString:[self configurationScript]]; _optionsDirty = NO; }
        const auto capture = _capture;
        const auto revision = _captureRevision;
        const auto identity = capture.get();
        if (!capture && _sentIdentity) [script appendString:@"window.fahSaver.setCapture(null);"];
        std::size_t sent = _sentFrames;
        if (capture && !capture->frames.empty()) {
            if (_sentIdentity != identity || _sentCapture.expired()) {
                fah::Capture first; first.topology = capture->topology;
                first.frames.push_back(_options.loop ? capture->frames.front() : capture->frames.back());
                const auto json = fah::writeCapture(first);
                [script appendFormat:@"window.fahSaver.setCapture(%s);",json.c_str()];
                sent = _options.loop ? 1 : capture->frames.size();
            } else if (!_options.loop && _sentRevision != revision) {
                nlohmann::json frame = nlohmann::json::array();
                for (auto position : capture->frames.back()) frame.push_back({position.x,position.y,position.z});
                [script appendFormat:@"window.fahSaver.replaceLatest(%s);",frame.dump().c_str()];
            } else if (_options.loop && sent < capture->frames.size()) {
                // Bound queued IPC; a large WU is transferred in chunks, never once per display frame.
                const auto count = std::max<std::size_t>(1,std::min<std::size_t>(16,1048576/(capture->topology.atoms.size()*48)));
                const auto end = std::min(capture->frames.size(),sent+count);
                nlohmann::json frames = nlohmann::json::array();
                for (; sent<end; ++sent) {
                    nlohmann::json frame = nlohmann::json::array();
                    for (auto position : capture->frames[sent]) frame.push_back({position.x,position.y,position.z});
                    frames.push_back(std::move(frame));
                }
                [script appendFormat:@"window.fahSaver.appendFrames(%s);",frames.dump().c_str()];
            }
        }
        if (_running && !_started) { [script appendString:@"window.fahSaver.start();"]; _started = YES; }
        if (_measurementPending) {
            [script appendString:@"window.fahSaver.beginMeasurement();"];
            _measurementPending = NO;
        }
        if (!script.length) return;
        [script appendString:@"window.fahSaver.metrics();"];
        _busy = YES;
        const NSUInteger generation = _generation;
        __weak FAHWebRenderer *weakSelf = self;
        [_webView evaluateJavaScript:script completionHandler:^(id result,NSError *error) {
            FAHWebRenderer *owner = weakSelf;
            if (!owner || owner->_generation != generation) return;
            owner->_busy = NO;
            if (error) { [owner fail:error.localizedDescription]; return; }
            if ([result isKindOfClass:NSDictionary.class]) owner->_javascriptMetrics = result;
            owner->_sentIdentity = identity; owner->_sentCapture = capture; owner->_sentFrames = sent; owner->_sentRevision = revision;
            if (owner.stateChanged) owner.stateChanged();
            [owner pump];
        }];
    } catch (const std::exception& exception) {
        [self fail:[NSString stringWithUTF8String:exception.what()]];
    }
}
- (void)beginMeasurement {
    _measuring = _measurementPending = YES;
    [self pump];
}
- (NSDictionary *)metrics {
    NSMutableDictionary *value = _javascriptMetrics ? [_javascriptMetrics mutableCopy] : [NSMutableDictionary new];
    value[@"renderer"] = @"WKWebView / WebGL2";
    value[@"ready"] = @(_ready); value[@"running"] = @(_running);
    value[@"pending_updates"] = @(_busy || _optionsDirty || _measurementPending || (_capture && (_sentIdentity != _capture.get() || _sentCapture.expired() ||
        (_options.loop ? _sentFrames < _capture->frames.size() : _sentRevision != _captureRevision))));
    value[@"submitted_frames"] = @([value[_measuring ? @"measurementDraws" : @"draws"] unsignedIntegerValue]);
    value[@"snapshot_count"] = @(_capture ? _capture->frames.size() : 0);
    value[@"snapshot_index"] = value[@"snapshotIndex"] ?: @0;
    value[@"atom_count"] = @(_capture ? _capture->topology.atoms.size() : 0);
    value[@"bond_count"] = @(_capture ? _capture->topology.bonds.size() : 0);
    value[@"requested_fps"] = @(_options.fps);
    value[@"snapshot_interval_seconds"] = @(_options.snapshotInterval);
    value[@"last_error"] = _lastError ?: @"";
    return value;
}
- (void)refreshMetrics:(void (^)(NSDictionary *))completion {
    if (!_ready || !_webView) { if (completion) completion([self metrics]); return; }
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    [_webView evaluateJavaScript:@"window.fahSaver.metrics()" completionHandler:^(id result,NSError *error) {
        FAHWebRenderer *owner = weakSelf;
        if (!owner || owner->_generation != generation) return;
        if (error) [owner fail:error.localizedDescription];
        else if ([result isKindOfClass:NSDictionary.class]) owner->_javascriptMetrics = result;
        if (completion) completion([owner metrics]);
    }];
}
- (void)renderAt:(double)seconds completion:(void (^)(NSDictionary *,NSError *))completion {
    if (!_ready || !_webView || !std::isfinite(seconds)) {
        if (completion) completion([self metrics],[NSError errorWithDomain:@"org.fahsaver.web-renderer" code:2 userInfo:@{NSLocalizedDescriptionKey:@"The Web renderer is not ready."}]);
        return;
    }
    NSString *script = [NSString stringWithFormat:@"window.fahSaver.renderAt(%.17g);window.fahSaver.metrics()",seconds];
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    [_webView evaluateJavaScript:script completionHandler:^(id result,NSError *error) {
        FAHWebRenderer *owner = weakSelf;
        if (!owner || owner->_generation != generation) {
            if (completion) completion(owner ? [owner metrics] : @{},[NSError errorWithDomain:@"org.fahsaver.web-renderer" code:3
                userInfo:@{NSLocalizedDescriptionKey:@"Rendering was cancelled because its page stopped."}]);
            return;
        }
        if ([result isKindOfClass:NSDictionary.class]) owner->_javascriptMetrics = result;
        if (completion) completion([owner metrics],error);
    }];
}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    if (!_webView || !message.frameInfo.isMainFrame || ![message.body isKindOfClass:NSDictionary.class]) return;
    NSDictionary *body = message.body;
    if ([body[@"type"] isEqual:@"ready"]) { _ready = YES; [self pump]; }
    else if ([body[@"type"] isEqual:@"error"]) [self fail:[body[@"message"] isKindOfClass:NSString.class] ? body[@"message"] : @"The Web renderer failed."];
    else if ([body[@"type"] isEqual:@"frame"]) {
        NSMutableDictionary *metrics = _javascriptMetrics ? [_javascriptMetrics mutableCopy] : [NSMutableDictionary new];
        [metrics addEntriesFromDictionary:body]; _javascriptMetrics = metrics;
    }
    if (self.stateChanged) self.stateChanged();
}
- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    const BOOL localPage = action.targetFrame.isMainFrame && [action.request.URL.URLByStandardizingPath isEqual:_pageURL];
    decisionHandler(localPage ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error { [self fail:error.localizedDescription]; }
- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error { [self fail:error.localizedDescription]; }
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView { [self fail:@"The Web renderer process stopped."]; }
- (void)dealloc { [self stop]; }
@end
