#import "FAHWebRenderer.h"
#import <os/log.h>
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

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
@interface FAHWebRenderer () <WKScriptMessageHandler, WKNavigationDelegate, WebFrameLoadDelegate, WebPolicyDelegate, WebResourceLoadDelegate, WebUIDelegate>
- (void)receiveRendererMessage:(NSDictionary *)body generation:(NSUInteger)generation;
- (void)fail:(NSString *)message;
@end

@interface FAHLegacyScriptHandler : NSObject
@property(nonatomic, weak) FAHWebRenderer *target;
@property(nonatomic) NSUInteger generation;
- (void)post:(NSString *)json;
@end
@implementation FAHLegacyScriptHandler
+ (BOOL)isSelectorExcludedFromWebScript:(SEL)selector { return selector != @selector(post:); }
+ (NSString *)webScriptNameForSelector:(SEL)selector { return selector == @selector(post:) ? @"post" : nil; }
+ (BOOL)isKeyExcludedFromWebScript:(const char *)name { return YES; }
- (void)post:(NSString *)json {
    if (![json isKindOfClass:NSString.class] || [json lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 65536) {
        [self.target fail:@"The Web renderer sent an invalid native message."]; return;
    }
    id body = [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    if (![body isKindOfClass:NSDictionary.class]) { [self.target fail:@"The Web renderer sent invalid JSON."]; return; }
    __weak FAHWebRenderer *target = self.target;
    const NSUInteger generation = self.generation;
    dispatch_async(dispatch_get_main_queue(), ^{ [target receiveRendererMessage:body generation:generation]; });
}
@end
@implementation FAHWebRenderer {
    NSURL *_assetsURL, *_pageURL;
    WKWebView *_webView;
    WebView *_legacyView;
    FAHWebBackend _backend;
    FAHWebClock _clock;
    FAHLegacyScriptHandler *_legacyHandler;
    FAHWeakScriptHandler *_handler;
    FAHRenderOptions _options;
    std::shared_ptr<const fah::Capture> _capture;
    std::weak_ptr<const fah::Capture> _sentCapture;
    const fah::Capture *_sentIdentity;
    std::size_t _sentFrames;
    NSUInteger _captureRevision, _sentRevision, _generation;
    BOOL _ready, _running, _started, _busy, _optionsDirty, _measuring, _measurementPending, _drawInFlight, _drawPending;
    double _pendingDrawTime, _lastAdvanceTime;
    NSUInteger _nativeDrawSubmitted, _nativeDrawCompleted;
    NSString *_lastError;
    NSDictionary *_javascriptMetrics;
}
- (void)logEvent:(const char *)event code:(NSInteger)code {
#if FAH_WEB_DIAGNOSTICS
    static os_log_t log = os_log_create("org.fahsaver.screensaver", "web-renderer");
    os_log(log,"renderer=%{public}p backend=%{public}s event=%{public}s code=%{public}ld ready=%{public}d running=%{public}d model=%{public}d busy=%{public}d window=%{public}d hidden=%{public}d width=%{public}.0f height=%{public}.0f native_submitted=%{public}lu native_completed=%{public}lu",
        (__bridge void *)self,_backend == FAHWebBackend::WK ? "wk" : "legacy",event,(long)code,_ready,_running,bool(_capture),_busy,self.presentationView.window != nil,
        self.presentationView.isHiddenOrHasHiddenAncestor,self.presentationView.bounds.size.width,self.presentationView.bounds.size.height,
        (unsigned long)_nativeDrawSubmitted,(unsigned long)_nativeDrawCompleted);
#endif
}
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL error:(NSError **)error {
    return [self initWithAssetsURL:assetsURL backend:FAHWebBackend::WK error:error];
}
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL backend:(FAHWebBackend)backend error:(NSError **)error {
    return [self initWithAssetsURL:assetsURL backend:backend clock:FAHWebClock::AnimationFrame error:error];
}
- (instancetype)initWithAssetsURL:(NSURL *)assetsURL backend:(FAHWebBackend)backend clock:(FAHWebClock)clock error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    if ((backend != FAHWebBackend::WK && backend != FAHWebBackend::Legacy) ||
        (clock != FAHWebClock::AnimationFrame && clock != FAHWebClock::Native)) {
        if (error) *error = [NSError errorWithDomain:@"org.fahsaver.web-renderer" code:5
            userInfo:@{NSLocalizedDescriptionKey:@"Unknown Web renderer backend or clock."}];
        return nil;
    }
    _backend = backend; _clock = clock;
    _assetsURL = assetsURL.URLByStandardizingPath;
    _pageURL = [_assetsURL URLByAppendingPathComponent:@"saver.html"];
    if (!_assetsURL.isFileURL || ![NSFileManager.defaultManager fileExistsAtPath:_pageURL.path]) {
        if (error) *error = [NSError errorWithDomain:@"org.fahsaver.web-renderer" code:1
            userInfo:@{NSLocalizedDescriptionKey:@"The bundled Web renderer is missing."}];
        return nil;
    }
    [self logEvent:"init" code:0];
    return self;
}
- (FAHWebClock)clock { return _clock; }
- (NSView *)presentationView { return _webView ?: (NSView *)_legacyView; }
- (WKWebView *)webView { return _webView; }
- (BOOL)ready { return _ready; }
- (NSString *)lastError { return _lastError; }
- (void)fail:(NSString *)message {
    _lastError = message.length ? message : @"The Web renderer failed.";
    [self logEvent:"error" code:0];
    if (self.stateChanged) self.stateChanged();
}
- (NSView *)prepareWithFrame:(NSRect)frame {
    if (self.presentationView) return self.presentationView;
    ++_generation;
    _ready = _busy = _started = _drawInFlight = _drawPending = NO;
    _lastAdvanceTime = -1;
    _nativeDrawSubmitted = _nativeDrawCompleted = 0;
    _measuring = _measurementPending;
    _javascriptMetrics = nil;
    _lastError = nil;
    _sentIdentity = nullptr; _sentFrames = _sentRevision = 0;
    _optionsDirty = YES;
    if (_backend == FAHWebBackend::WK) {
        WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
        configureSharedSession(configuration);
        _handler = [FAHWeakScriptHandler new]; _handler.target = self;
        [configuration.userContentController addScriptMessageHandler:_handler name:@"renderer"];
#if FAH_WEB_DIAGNOSTICS
        [configuration.userContentController addUserScript:[[WKUserScript alloc] initWithSource:[self visibilityScript]
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
#endif
        _webView = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
        _webView.navigationDelegate = self;
        _webView.allowsBackForwardNavigationGestures = NO;
        [_webView loadFileURL:_pageURL allowingReadAccessToURL:_assetsURL];
    } else {
        WebPreferences *preferences = [[WebPreferences alloc] initWithIdentifier:
            [@"org.fahsaver.private." stringByAppendingString:NSUUID.UUID.UUIDString]];
        preferences.autosaves = NO;
        preferences.privateBrowsingEnabled = YES;
        preferences.javaScriptCanOpenWindowsAutomatically = NO;
        preferences.plugInsEnabled = NO;
        _legacyView = [[WebView alloc] initWithFrame:frame frameName:nil groupName:nil];
        _legacyView.preferences = preferences;
        _legacyHandler = [FAHLegacyScriptHandler new]; _legacyHandler.target = self; _legacyHandler.generation = _generation;
        _legacyView.frameLoadDelegate = self;
        _legacyView.policyDelegate = self;
        _legacyView.resourceLoadDelegate = self;
        _legacyView.UIDelegate = self;
        [_legacyView.mainFrame loadRequest:[NSURLRequest requestWithURL:_pageURL]];
    }
    self.presentationView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self logEvent:"prepare" code:0];
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        FAHWebRenderer *owner = weakSelf;
        if (owner && owner->_generation == generation && owner.presentationView && !owner->_ready)
            [owner fail:@"The Web renderer did not become ready."];
    });
    return self.presentationView;
}
- (NSString *)visibilityScript {
    return @"(()=>{const post=()=>window.webkit?.messageHandlers?.renderer?.postMessage({type:'visibility',hidden:document.hidden,width:innerWidth,height:innerHeight,dpr:devicePixelRatio});addEventListener('pageshow',post);document.addEventListener('visibilitychange',post);})()";
}
- (void)evaluateRuntimeScript:(NSString *)script completion:(void (^)(id,NSError *))completion {
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    void (^finish)(id,NSError *) = ^(id value,NSError *error) {
        if (!completion) return;
        FAHWebRenderer *owner = weakSelf;
        if (!owner || owner->_generation != generation) {
            completion(nil,[NSError errorWithDomain:@"org.fahsaver.web-renderer" code:3
                userInfo:@{NSLocalizedDescriptionKey:@"JavaScript was cancelled because its page stopped."}]);
        } else completion(value,error);
    };
    if (_webView) { [_webView evaluateJavaScript:script completionHandler:finish]; return; }
    id value = nil;
    NSError *error = nil;
    @autoreleasepool {
    try { @try {
        if (!_legacyView) throw std::runtime_error("The Web renderer has no page");
        WebScriptObject *window = _legacyView.windowScriptObject;
        value = [window evaluateWebScript:script];
        if ([value isKindOfClass:WebUndefined.class]) throw std::runtime_error("Legacy JavaScript evaluation failed");
        if ([value isKindOfClass:WebScriptObject.class]) {
            WebScriptObject *json = [window valueForKey:@"JSON"];
            NSString *text = [json callWebScriptMethod:@"stringify" withArguments:@[value]];
            if (![text isKindOfClass:NSString.class]) throw std::runtime_error("Legacy JavaScript result was not JSON");
            value = [NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding]
                options:NSJSONReadingFragmentsAllowed error:&error];
        }
        if (!value && !error) value = NSNull.null;
    } @catch (NSException *exception) {
        error = [NSError errorWithDomain:@"org.fahsaver.web-renderer" code:4
            userInfo:@{NSLocalizedDescriptionKey:@"Legacy JavaScript evaluation failed."}];
    }} catch (const std::exception& exception) {
        error = [NSError errorWithDomain:@"org.fahsaver.web-renderer" code:4
            userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithUTF8String:exception.what()]}];
    }
    dispatch_async(dispatch_get_main_queue(), ^{ finish(value,error); });
    }
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
- (void)start {
    if (!_running) _lastAdvanceTime = -1;
    _running = YES; [self logEvent:"start" code:0]; [self pump];
}
- (BOOL)advanceTo:(double)seconds {
    if (_clock != FAHWebClock::Native || !_running || _lastError || !std::isfinite(seconds) || seconds < 0 || seconds < _lastAdvanceTime)
        return NO;
    _lastAdvanceTime = _pendingDrawTime = seconds; _drawPending = YES;
    [self pumpDraw];
    return YES;
}
- (void)pumpDraw {
    if (!_drawPending || !_running || !_ready || !self.presentationView || !_capture || _busy || _drawInFlight || _lastError)
        return;
    _drawPending = NO; _drawInFlight = YES; ++_nativeDrawSubmitted;
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    NSString *script = [NSString stringWithFormat:@"window.fahSaver.advanceTo(%.17g)",_pendingDrawTime];
    [self evaluateRuntimeScript:script completion:^(id result,NSError *error) {
        FAHWebRenderer *owner = weakSelf;
        if (!owner || owner->_generation != generation) return;
        owner->_drawInFlight = NO;
        if (error) { [owner logEvent:"native-draw-error" code:error.code]; [owner fail:error.localizedDescription]; return; }
        if (![result respondsToSelector:@selector(boolValue)]) {
            [owner fail:@"The Web renderer returned an invalid frame result."]; return;
        }
        ++owner->_nativeDrawCompleted;
        if (owner->_nativeDrawCompleted == 1) [owner logEvent:"first-native-advance" code:[result boolValue]];
        [owner pump]; [owner pumpDraw];
    }];
}
- (void)stop {
    [self logEvent:"stop" code:0];
    _drawInFlight = _drawPending = NO;
    _running = _ready = _started = _busy = _measurementPending = NO; ++_generation;
    if (_legacyView) {
        [_legacyView stringByEvaluatingJavaScriptFromString:@"window.fahSaver && window.fahSaver.dispose()"];
        _legacyHandler.target = nil;
        _legacyView.frameLoadDelegate = nil; _legacyView.policyDelegate = nil;
        _legacyView.resourceLoadDelegate = nil; _legacyView.UIDelegate = nil;
        [_legacyView stopLoading:nil]; [_legacyView removeFromSuperview]; [_legacyView close];
        _legacyView = nil; _legacyHandler = nil;
    }
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
        @"collectMetrics":@(_options.collectMetrics),
        @"frameDriver":_clock == FAHWebClock::Native ? @"native" : @"raf" };
    NSData *json = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    return [NSString stringWithFormat:@"window.fahSaver.configure(%@);",[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding]];
}
- (void)pump {
    if (!_ready || !self.presentationView || _busy || _drawInFlight || _lastError) return;
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
                if (_clock == FAHWebClock::Native && _started && !_sentIdentity && _drawPending) {
                    [script appendFormat:@"window.fahSaver.advanceTo(%.17g);",_pendingDrawTime];
                    _drawPending = NO;
                }
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
        if (_running && !_started) {
            [script appendString:@"window.fahSaver.start();"];
            _started = YES;
        }
        if (_measurementPending) {
            [script appendString:@"window.fahSaver.beginMeasurement();"];
            _measurementPending = NO;
        }
        if (!script.length) return;
        [script appendString:@"window.fahSaver.metrics();"];
        _busy = YES;
        const NSUInteger generation = _generation;
        __weak FAHWebRenderer *weakSelf = self;
        [self evaluateRuntimeScript:script completion:^(id result,NSError *error) {
            FAHWebRenderer *owner = weakSelf;
            if (!owner || owner->_generation != generation) return;
            owner->_busy = NO;
            if (error) { [owner fail:error.localizedDescription]; return; }
            if ([result isKindOfClass:NSDictionary.class]) owner->_javascriptMetrics = result;
            if (owner->_sentIdentity != identity && identity) [owner logEvent:"model-transferred" code:0];
            owner->_sentIdentity = identity; owner->_sentCapture = capture; owner->_sentFrames = sent; owner->_sentRevision = revision;
            if (owner.stateChanged) owner.stateChanged();
            [owner pump]; [owner pumpDraw];
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
    value[@"renderer"] = _backend == FAHWebBackend::WK ? @"WKWebView / WebGL2" : @"WebView / WebGL2";
    value[@"backend"] = _backend == FAHWebBackend::WK ? @"wk" : @"legacy";
    value[@"frame_clock"] = _clock == FAHWebClock::Native ? @"native" : @"raf";
    value[@"pending_native_frame"] = @(_drawPending || _drawInFlight);
    value[@"native_draw_submitted"] = @(_nativeDrawSubmitted);
    value[@"native_draw_completed"] = @(_nativeDrawCompleted);
    value[@"ready"] = @(_ready); value[@"running"] = @(_running);
    value[@"bridge_busy"] = @(_busy);
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
    if (!_ready || !self.presentationView) { if (completion) completion([self metrics]); return; }
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    [self evaluateRuntimeScript:@"window.fahSaver.metrics()" completion:^(id result,NSError *error) {
        FAHWebRenderer *owner = weakSelf;
        if (!owner || owner->_generation != generation) return;
        if (error) [owner fail:error.localizedDescription];
        else if ([result isKindOfClass:NSDictionary.class]) owner->_javascriptMetrics = result;
        if (completion) completion([owner metrics]);
    }];
}
- (void)renderAt:(double)seconds completion:(void (^)(NSDictionary *,NSError *))completion {
    if (!_ready || !self.presentationView || !std::isfinite(seconds)) {
        if (completion) completion([self metrics],[NSError errorWithDomain:@"org.fahsaver.web-renderer" code:2 userInfo:@{NSLocalizedDescriptionKey:@"The Web renderer is not ready."}]);
        return;
    }
    NSString *script = [NSString stringWithFormat:@"window.fahSaver.renderAt(%.17g);window.fahSaver.metrics()",seconds];
    const NSUInteger generation = _generation;
    __weak FAHWebRenderer *weakSelf = self;
    [self evaluateRuntimeScript:script completion:^(id result,NSError *error) {
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
    if (message.webView != _webView || !message.frameInfo.isMainFrame || ![message.body isKindOfClass:NSDictionary.class]) return;
    [self receiveRendererMessage:message.body generation:_generation];
}
- (void)receiveRendererMessage:(NSDictionary *)body generation:(NSUInteger)generation {
    if (!self.presentationView || generation != _generation) return;
    if ([body[@"type"] isEqual:@"ready"]) { _ready = YES; [self logEvent:"ready" code:0]; [self pump]; }
    else if ([body[@"type"] isEqual:@"error"]) { [self logEvent:"javascript-error" code:0]; [self fail:[body[@"message"] isKindOfClass:NSString.class] ? body[@"message"] : @"The Web renderer failed."]; }
    else if ([body[@"type"] isEqual:@"frame"]) {
        // One message per start verifies remote-host backing dimensions without a polling timer.
        static os_log_t log = os_log_create("org.fahsaver.screensaver", "web-renderer");
        os_log_info(log, "First model frame: %lu x %lu; draw count %lu",
            [body[@"width"] unsignedLongValue], [body[@"height"] unsignedLongValue], [body[@"draws"] unsignedLongValue]);
        [self logEvent:"first-frame" code:0];
        NSMutableDictionary *metrics = _javascriptMetrics ? [_javascriptMetrics mutableCopy] : [NSMutableDictionary new];
        [metrics addEntriesFromDictionary:body]; _javascriptMetrics = metrics;
    }
#if FAH_WEB_DIAGNOSTICS
    else if ([body[@"type"] isEqual:@"visibility"]) {
        static os_log_t log = os_log_create("org.fahsaver.screensaver", "web-renderer");
        os_log(log,"renderer=%{public}p event=visibility hidden=%{public}d width=%{public}.0f height=%{public}.0f dpr=%{public}.2f",
            (__bridge void *)self,[body[@"hidden"] boolValue],[body[@"width"] doubleValue],
            [body[@"height"] doubleValue],[body[@"dpr"] doubleValue]);
    }
#endif
    if (self.stateChanged) self.stateChanged();
}
- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    const BOOL localPage = action.targetFrame.isMainFrame && [action.request.URL.URLByStandardizingPath isEqual:_pageURL];
    [self logEvent:localPage ? "navigation-allow" : "navigation-deny" code:0];
    decisionHandler(localPage ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation { [self logEvent:"navigation-start" code:0]; }
- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation { [self logEvent:"navigation-commit" code:0]; }
- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation { [self logEvent:"navigation-finish" code:0]; }
- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error { [self logEvent:"navigation-fail" code:error.code]; [self fail:error.localizedDescription]; }
- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error { [self logEvent:"navigation-fail" code:error.code]; [self fail:error.localizedDescription]; }
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView { [self logEvent:"process-terminated" code:0]; [self fail:@"The Web renderer process stopped."]; }
- (void)webView:(WebView *)view didClearWindowObject:(WebScriptObject *)window forFrame:(WebFrame *)frame {
    if (view != _legacyView || frame != view.mainFrame) return;
    [window setValue:_legacyHandler forKey:@"fahNativeRendererBridge"];
    id installed = [window evaluateWebScript:@"Object.defineProperty(window,'webkit',{value:{messageHandlers:{renderer:{postMessage:value=>window.fahNativeRendererBridge.post(JSON.stringify(value))}}}});typeof window.fahNativeRendererBridge.post==='function'"];
    if (![installed respondsToSelector:@selector(boolValue)] || ![installed boolValue]) {
        [self fail:@"The native renderer bridge could not be installed."]; return;
    }
    [self logEvent:"bridge-installed" code:0];
#if FAH_WEB_DIAGNOSTICS
    [window evaluateWebScript:[self visibilityScript]];
#endif
}
- (void)webView:(WebView *)view didStartProvisionalLoadForFrame:(WebFrame *)frame { if (frame == view.mainFrame) [self logEvent:"navigation-start" code:0]; }
- (void)webView:(WebView *)view didCommitLoadForFrame:(WebFrame *)frame { if (frame == view.mainFrame) [self logEvent:"navigation-commit" code:0]; }
- (void)webView:(WebView *)view didFinishLoadForFrame:(WebFrame *)frame { if (frame == view.mainFrame) [self logEvent:"navigation-finish" code:0]; }
- (void)webView:(WebView *)view didFailLoadWithError:(NSError *)error forFrame:(WebFrame *)frame { [self logEvent:"navigation-fail" code:error.code]; [self fail:error.localizedDescription]; }
- (void)webView:(WebView *)view didFailProvisionalLoadWithError:(NSError *)error forFrame:(WebFrame *)frame { [self logEvent:"navigation-fail" code:error.code]; [self fail:error.localizedDescription]; }
- (void)webView:(WebView *)view decidePolicyForNavigationAction:(NSDictionary *)action request:(NSURLRequest *)request frame:(WebFrame *)frame decisionListener:(id<WebPolicyDecisionListener>)listener {
    const BOOL local = frame == view.mainFrame && [request.URL.URLByStandardizingPath isEqual:_pageURL];
    [self logEvent:local ? "navigation-allow" : "navigation-deny" code:0];
    if (local) [listener use]; else [listener ignore];
}
- (NSURLRequest *)webView:(WebView *)view resource:(id)identifier willSendRequest:(NSURLRequest *)request redirectResponse:(NSURLResponse *)redirect fromDataSource:(WebDataSource *)source {
    NSURL *url = request.URL.URLByResolvingSymlinksInPath;
    NSString *directory = [[_assetsURL URLByResolvingSymlinksInPath].path stringByStandardizingPath];
    const BOOL local = url.isFileURL && [[[url.path stringByDeletingLastPathComponent] stringByStandardizingPath] isEqual:directory];
    if (!local) [self logEvent:"resource-deny" code:0];
    return local ? request : nil;
}
- (WebView *)webView:(WebView *)view createWebViewWithRequest:(NSURLRequest *)request { return nil; }
- (void)dealloc { [self stop]; }
@end

#pragma clang diagnostic pop
