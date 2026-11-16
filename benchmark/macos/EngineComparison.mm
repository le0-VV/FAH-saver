#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CommonCrypto/CommonDigest.h>
#import "FAHRenderer.h"
#import "FAHWebRenderer.h"
#include "fah/Model.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

static double now() { return CACurrentMediaTime(); }
static void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
static bool boolean(id value) { return value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID(); }
static void keys(NSDictionary *value, NSArray<NSString *> *allowed) {
    for (id key in value) require([allowed containsObject:key],"Unsupported comparison configuration key");
}
static double numeric(NSDictionary *value, NSString *key, double fallback, double minimum, double maximum) {
    id item = value[key]; if (!item) return fallback;
    require([item isKindOfClass:NSNumber.class] && !boolean(item),"Configuration needs a numeric value");
    const double result = [item doubleValue];
    require(std::isfinite(result) && result >= minimum && result <= maximum,"Configuration number is outside its finite range");
    return result;
}
static NSInteger integer(NSDictionary *value, NSString *key, NSInteger fallback, NSInteger minimum, NSInteger maximum) {
    const double result = numeric(value,key,fallback,minimum,maximum);
    require(result == std::floor(result),"Configuration needs an integer value"); return static_cast<NSInteger>(result);
}
static NSString *text(NSDictionary *value, NSString *key, NSString *fallback) {
    id item = value[key]; if (!item) return fallback;
    require([item isKindOfClass:NSString.class] && [item length] > 0 && [item length] <= 4096,
            "Configuration needs nonempty text"); return item;
}
static NSString *path(NSDictionary *value, NSString *key, bool required) {
    NSString *result = text(value,key,nil);
    require(!required || result != nil,"Configuration needs an explicit file path");
    if (result) require(result.isAbsolutePath,"Comparison paths must be absolute");
    return result.stringByStandardizingPath;
}
static void writeJSON(NSDictionary *value, NSString *file) {
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingPrettyPrinted error:&error];
    require(data && [data writeToFile:file options:NSDataWritingAtomic error:&error],"Could not write comparison JSON");
}
static NSString *sha256(NSData *data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(data.bytes,static_cast<CC_LONG>(data.length),digest);
    NSMutableString *result = [NSMutableString new]; for (unsigned char byte : digest) [result appendFormat:@"%02x",byte]; return result;
}

@interface ECConfig : NSObject {
@public
    NSString *engine, *webBackend, *webClock, *output, *status, *mode, *screenshot, *background, *captureHash;
    NSURL *assets;
    NSArray<NSDictionary *> *layouts;
    FAHRenderOptions options;
    double warmup, measure, cooldown, fixedTime;
    BOOL hiddenReference;
    std::shared_ptr<const fah::Capture> capture;
}
@end
@implementation ECConfig @end

static ECConfig *configuration(NSDictionary *raw) {
    require([raw isKindOfClass:NSDictionary.class],"Comparison configuration must be an object");
    keys(raw,@[@"engine",@"webBackend",@"webClock",@"capture",@"assets",@"width",@"height",@"layouts",@"warmupSeconds",@"measureSeconds",
               @"cooldownSeconds",@"style",@"fps",@"playback",@"snapshotIntervalSeconds",@"background",@"colour",
               @"collectMetrics",@"output",@"status",@"mode",@"screenshot",@"fixedTimeSeconds"]);
    ECConfig *c = [ECConfig new];
    c->engine = text(raw,@"engine",nil); require(c->engine&&[@[@"metal",@"web"] containsObject:c->engine],"Engine must be metal or web");
    c->webBackend = text(raw,@"webBackend",@"wk");
    require([@[@"wk",@"legacy"] containsObject:c->webBackend],"Web backend must be wk or legacy");
    c->webClock = text(raw,@"webClock",@"raf");
    require([@[@"raf",@"native"] containsObject:c->webClock],"Web clock must be raf or native");
    c->mode = text(raw,@"mode",@"benchmark"); require([@[@"benchmark",@"screenshot"] containsObject:c->mode],"Mode must be benchmark or screenshot");
    NSString *capturePath = path(raw,@"capture",true); c->output = path(raw,@"output",true); c->status = path(raw,@"status",true);
    c->screenshot = path(raw,@"screenshot",[c->mode isEqual:@"screenshot"]);
    require(!c->screenshot || [c->mode isEqual:@"screenshot"],"Screenshots require screenshot mode");
    NSMutableSet *paths = [NSMutableSet setWithObject:capturePath.stringByResolvingSymlinksInPath];
    for (NSString *file in @[c->output,c->status,c->screenshot ?: @""]) if (file.length) {
        NSString *resolved = file.stringByResolvingSymlinksInPath;
        require(![paths containsObject:resolved],"Input and output paths must be distinct"); [paths addObject:resolved];
        BOOL directory = NO; require([NSFileManager.defaultManager fileExistsAtPath:file.stringByDeletingLastPathComponent isDirectory:&directory] && directory,
                                    "Output directories must already exist");
    }
    NSString *assetsPath = path(raw,@"assets",false);
    c->assets = assetsPath ? [NSURL fileURLWithPath:assetsPath isDirectory:YES] :
        [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"web" isDirectory:YES];
    if ([c->engine isEqual:@"web"]) require([NSFileManager.defaultManager fileExistsAtPath:[c->assets URLByAppendingPathComponent:@"saver.html"].path],
                                            "Web assets must contain saver.html");
    id inputLayouts = raw[@"layouts"];
    if (inputLayouts) require(!raw[@"width"] && !raw[@"height"],"Use layouts or width/height, not both");
    else inputLayouts = @[@{@"width":raw[@"width"] ?: @2880,@"height":raw[@"height"] ?: @1800}];
    require([inputLayouts isKindOfClass:NSArray.class] && [inputLayouts count] >= 1 && [inputLayouts count] <= 3,
            "Use one to three comparison layouts");
    NSMutableArray *layouts = [NSMutableArray new];
    for (id item in inputLayouts) {
        require([item isKindOfClass:NSDictionary.class],"Each layout must be an object"); keys(item,@[@"width",@"height",@"screen"]);
        [layouts addObject:@{@"width":@(integer(item,@"width",2880,64,4096)),@"height":@(integer(item,@"height",1800,64,4096)),
                            @"screen":@(integer(item,@"screen",0,0,15))}];
    }
    c->layouts = layouts; require(![c->mode isEqual:@"screenshot"] || layouts.count == 1,"Screenshot mode requires one view");
    c->warmup = numeric(raw,@"warmupSeconds",10,0,120); c->measure = numeric(raw,@"measureSeconds",30,.01,600);
    c->cooldown = numeric(raw,@"cooldownSeconds",10,0,120); c->fixedTime = numeric(raw,@"fixedTimeSeconds",0,0,3600);
    c->options.fps = static_cast<int>(integer(raw,@"fps",60,1,240));
    c->options.style = static_cast<fah::Style>(integer(raw,@"style",1,1,3));
    NSString *playback = text(raw,@"playback",@"latest"); require([@[@"latest",@"loop"] containsObject:playback],"Playback must be latest or loop");
    c->options.loop = [playback isEqual:@"loop"];
    c->options.snapshotInterval = numeric(raw,@"snapshotIntervalSeconds",1,.05,DBL_MAX);
    c->background = text(raw,@"background",@"dark"); require([@[@"light",@"dark",@"solid"] containsObject:c->background],"Background must be light, dark or solid");
    c->options.sky = ![c->background isEqual:@"solid"]; c->options.darkSky = [c->background isEqual:@"dark"];
    if (raw[@"colour"]) {
        id colour = raw[@"colour"]; require([colour isKindOfClass:NSArray.class] && [colour count] == 3,"Colour needs three components");
        float values[3]; for (NSUInteger i=0;i<3;++i) values[i] = static_cast<float>(numeric(@{@"v":colour[i]},@"v",0,0,1));
        c->options.colour = {values[0],values[1],values[2]};
    }
    if (raw[@"collectMetrics"]) require(boolean(raw[@"collectMetrics"]),"collectMetrics must be a boolean");
    c->options.collectMetrics = [raw[@"collectMetrics"] boolValue];
    NSData *data = [NSData dataWithContentsOfFile:capturePath]; require(data && data.length <= 32u*1024*1024,"Could not read a bounded fixture");
    try { c->capture = std::make_shared<const fah::Capture>(fah::parseCapture(std::string_view(static_cast<const char*>(data.bytes),data.length))); }
    catch (const std::exception&) { throw std::runtime_error("Fixture must contain valid bounded protein data"); }
    c->captureHash = sha256(data); return c;
}
static ECConfig *readConfiguration(NSString *file) {
    require(file.isAbsolutePath,"Comparison configuration path must be absolute");
    NSData *data = [NSData dataWithContentsOfFile:file]; require(data && data.length <= 65536,"Could not read a bounded comparison configuration");
    NSError *error = nil; NSDictionary *raw = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    require(raw && !error,"Comparison configuration is invalid JSON"); return configuration(raw);
}
static void referenceMode(ECConfig *config) {
    require([config->mode isEqual:@"screenshot"],"--render-reference requires screenshot mode");
    config->hiddenReference=YES;
}

@class ECHarness;
@interface ECWindow : NSWindow
@property(copy) void (^escapeHandler)(void);
@end
@implementation ECWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (NSRect)constrainFrameRect:(NSRect)frame toScreen:(NSScreen *)screen { (void)screen; return frame; }
- (void)sendEvent:(NSEvent *)event {
    if (event.type == NSEventTypeKeyDown && event.keyCode == 53) { if (self.escapeHandler) self.escapeHandler(); return; }
    [super sendEvent:event];
}
@end
@interface ECMetalDriver : NSObject <MTKViewDelegate>
@property(strong) FAHRenderer *renderer;
@property NSInteger width, height;
@property unsigned long long submissions;
@property(copy) void (^changed)(NSString *error);
@end
@implementation ECMetalDriver
- (void)drawInMTKView:(MTKView *)view {
    @autoreleasepool {
        id<CAMetalDrawable> drawable = view.currentDrawable; if (!drawable) return;
        if (drawable.texture.width != static_cast<NSUInteger>(self.width) || drawable.texture.height != static_cast<NSUInteger>(self.height)) {
            if (self.changed) self.changed(@"Metal drawable dimensions changed"); return;
        }
        id<MTLCommandBuffer> command = [self.renderer renderToTexture:drawable.texture time:now() drawable:drawable];
        if (command) ++_submissions;
        if (self.changed) self.changed(self.renderer.lastError);
    }
}
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; }
@end
@interface ECCanvas : NSObject
@property(strong) ECWindow *window;
@property(strong) NSView *presentation;
@property(strong) FAHRenderer *metal;
@property(strong) ECMetalDriver *driver;
@property(strong) FAHWebRenderer *web;
@property(strong) NSDictionary *layout, *baseline, *finalMetrics, *failureState;
@property double scale, baselineUptime, finalUptime, clockEpoch;
@property NSInteger actualWidth, actualHeight;
@property unsigned long long baselineDraws, finalDraws;
@property unsigned long long nativeClockRequests, nativeClockAccepted;
@property unsigned long long baselineClockRequests, baselineClockAccepted, finalClockRequests, finalClockAccepted;
@property BOOL ready, started, sizing, finalMetricsFromFailureCache;
@end
@implementation ECCanvas @end

@interface ECHarness : NSObject <NSApplicationDelegate,NSWindowDelegate> {
    ECConfig *_config;
    NSMutableArray<ECCanvas *> *_canvases;
    NSMutableDictionary *_times;
    NSMutableArray<NSMutableDictionary *> *_loadingDiagnostics;
    NSString *_phase, *_reason, *_failure;
    NSTimer *_drawTimer, *_loadingTimer, *_phaseTimer, *_hardTimer, *_collectionTimer, *_diagnosticTimer, *_diagnosticDeadline;
    BOOL _finishing, _cancelled, _terminal;
    int _exitCode;
}
- (instancetype)initWithConfig:(ECConfig *)config;
- (void)fail:(NSString *)message;
- (void)cancel:(NSString *)reason;
@property(readonly) int exitCode;
@end
static void protect(ECHarness *owner, void (^action)(void)) {
    try { action(); } catch (const std::exception& error) { [owner fail:[NSString stringWithUTF8String:error.what()]]; }
}
static NSNumber *draws(NSDictionary *metrics) {
    id value = metrics[@"draws"]; require([value isKindOfClass:NSNumber.class],"Web draw counter is unavailable"); return value;
}
@implementation ECHarness
- (instancetype)initWithConfig:(ECConfig *)config { self=[super init]; if(self){_config=config;_canvases=[NSMutableArray new];_times=[NSMutableDictionary new];_phase=@"loading";} return self; }
- (int)exitCode { return _exitCode; }
- (NSDictionary *)statusRecord {
    NSMutableDictionary *value = [@{@"pid":@(getpid()),@"bundle_id":NSBundle.mainBundle.bundleIdentifier ?: @"",
        @"engine":_config->engine,@"web_backend":[_config->engine isEqual:@"web"]?_config->webBackend:@"not_applicable",
        @"web_clock":[_config->engine isEqual:@"web"]?_config->webClock:@"not_applicable",
        @"execution_mode":_config->hiddenReference?@"hidden_reference_cli":@"windowed_app",
        @"phase":_phase,@"uptime":@(now()),@"unix_time":@([NSDate date].timeIntervalSince1970)} mutableCopy];
    [value addEntriesFromDictionary:_times]; if(_failure)value[@"error"]=_failure; if(_cancelled)value[@"cancelled"]=@YES;
    if(_loadingDiagnostics)value[@"loading_diagnostics"]=_loadingDiagnostics;return value;
}
- (NSDictionary *)cachedState:(ECCanvas *)canvas {
    const NSRect bounds=canvas.presentation?canvas.presentation.bounds:NSZeroRect;
    return @{@"uptime":@(now()),@"harness_view_ready":@(canvas.ready),@"harness_start_requested":@(canvas.started),
        @"harness_sizing_requested":@(canvas.sizing),@"window_visible":@(canvas.window.visible),
        @"presentation_present":@(canvas.presentation!=nil),@"presentation_hidden":@(canvas.presentation.isHiddenOrHasHiddenAncestor),
        @"presentation_has_window":@(canvas.presentation.window!=nil),@"presentation_width_points":@(bounds.size.width),
        @"presentation_height_points":@(bounds.size.height),@"metal_submissions":@(canvas.driver.submissions),
        @"native_clock_requests":@(canvas.nativeClockRequests),@"native_clock_accepted":@(canvas.nativeClockAccepted),
        @"metrics":canvas.web?canvas.web.metrics:(canvas.metal.metrics?:@{})};
}
- (void)publishLoadingDiagnostics {
    if(!_terminal&&[_phase isEqual:@"loading"])protect(self,^{writeJSON([self statusRecord],self->_config->status);});
}
- (void)captureLoadingDiagnostics {
    _diagnosticTimer=nil;
    if(_terminal||_finishing||![_phase isEqual:@"loading"])return;
    _times[@"loading_diagnostic_uptime"]=@(now());_loadingDiagnostics=[NSMutableArray new];
    __weak ECHarness *weakSelf=self;
    for(ECCanvas *canvas in _canvases) {
        NSMutableDictionary *entry=[@{@"view_index":@(_loadingDiagnostics.count),@"cached_state":[self cachedState:canvas],
            @"query_state":canvas.web?@"pending":@"not_applicable"} mutableCopy];[_loadingDiagnostics addObject:entry];
        if(!canvas.web)continue;
        NSString *script=@"(()=>{const c=document.querySelector('canvas');const r=window.fahSaver;const v={document_ready_state:document.readyState,document_hidden:document.hidden,document_visibility_state:document.visibilityState,device_pixel_ratio:window.devicePixelRatio,canvas_present:!!c,canvas_width:c?c.width:null,canvas_height:c?c.height:null,canvas_client_width:c?c.clientWidth:null,canvas_client_height:c?c.clientHeight:null,runtime_present:!!r};try{v.runtime_metrics=r?r.metrics():null;}catch(e){v.runtime_metrics_failed=true;}return v;})()";
        @autoreleasepool {[canvas.web evaluateRuntimeScript:script completion:^(id result,NSError *error){@autoreleasepool {
            ECHarness *owner=weakSelf;if(!owner||owner->_terminal||![entry[@"query_state"] isEqual:@"pending"])return;
            entry[@"query_completed_uptime"]=@(now());
            if(error){entry[@"query_state"]=@"error";entry[@"error_domain"]=error.domain;entry[@"error_code"]=@(error.code);}
            else if([result isKindOfClass:NSDictionary.class]){entry[@"query_state"]=@"complete";entry[@"javascript_state"]=result;}
            else entry[@"query_state"]=@"invalid_result";
            [owner publishLoadingDiagnostics];
        }}];}
    }
    [self publishLoadingDiagnostics];
    _diagnosticDeadline=[NSTimer scheduledTimerWithTimeInterval:3 repeats:NO block:^(NSTimer *timer){(void)timer;ECHarness *owner=weakSelf;if(!owner||owner->_terminal)return;
        for(NSMutableDictionary *entry in owner->_loadingDiagnostics)if([entry[@"query_state"] isEqual:@"pending"]){entry[@"query_state"]=@"timeout";entry[@"query_timeout_uptime"]=@(now());}
        owner->_diagnosticDeadline=nil;[owner publishLoadingDiagnostics];}];
}
- (void)phase:(NSString *)phase timestamp:(NSString *)key {
    _phase=phase; if(key)_times[key]=@(now()); writeJSON([self statusRecord],_config->status);
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    @autoreleasepool { protect(self, ^{ [self launch]; }); }
}
- (void)launch {
    [self phase:@"loading" timestamp:@"loading_start_uptime"];
    __weak ECHarness *weakSelf=self;
    const double activeBudget=[_config->mode isEqual:@"screenshot"]?0:_config->warmup+_config->measure;
    _hardTimer=[NSTimer scheduledTimerWithTimeInterval:45+activeBudget+_config->cooldown repeats:NO block:^(NSTimer *timer){(void)timer;[weakSelf fail:@"Comparison exceeded its bounded lifetime"];}];
    _diagnosticTimer=[NSTimer scheduledTimerWithTimeInterval:5 repeats:NO block:^(NSTimer *timer){(void)timer;ECHarness *owner=weakSelf;if(owner)protect(owner,^{[owner captureLoadingDiagnostics];});}];
    NSArray<NSScreen *> *screens=NSScreen.screens; require(screens.count>0,"No display is available");
    for(NSDictionary *layout in _config->layouts) {
        NSUInteger index=[layout[@"screen"] unsignedIntegerValue]; require(index<screens.count,"Requested display is unavailable");
        NSScreen *screen=screens[index]; const double scale=screen.backingScaleFactor;
        const CGFloat width=[layout[@"width"] doubleValue]/scale,height=[layout[@"height"] doubleValue]/scale;
        ECCanvas *canvas=[ECCanvas new]; canvas.layout=layout; canvas.scale=scale;
        canvas.window=[[ECWindow alloc] initWithContentRect:NSMakeRect(0,0,width,height)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskFullSizeContentView
            backing:NSBackingStoreBuffered defer:NO screen:screen];
        canvas.window.releasedWhenClosed=NO;
        canvas.window.title=[NSString stringWithFormat:@"FAH Engine Comparison — %@%@",_config->engine,
            [_config->engine isEqual:@"web"]?[@" / " stringByAppendingString:_config->webBackend]:@""];
        canvas.window.titlebarAppearsTransparent=YES; canvas.window.titleVisibility=NSWindowTitleHidden;
        canvas.window.delegate=self; canvas.window.escapeHandler=^{[weakSelf cancel:@"escape"];};
        const CGFloat offset=12*_canvases.count;
        [canvas.window setFrame:NSMakeRect(screen.frame.origin.x+offset,screen.frame.origin.y+offset,width,height) display:NO];
        if([_config->engine isEqual:@"metal"]) {
            NSError *error=nil; canvas.metal=[[FAHRenderer alloc] initWithDevice:MTLCreateSystemDefaultDevice() error:&error];
            require(canvas.metal!=nil,"Metal renderer could not be created");
            [canvas.metal setOptions:_config->options]; [canvas.metal setCapture:_config->capture]; [canvas.metal start];
            MTKView *view=[[MTKView alloc] initWithFrame:NSMakeRect(0,0,width,height) device:canvas.metal.device];
            view.paused=YES;view.enableSetNeedsDisplay=NO;view.framebufferOnly=YES;view.colorPixelFormat=MTLPixelFormatBGRA8Unorm;
            canvas.driver=[ECMetalDriver new];canvas.driver.renderer=canvas.metal;
            canvas.driver.width=[layout[@"width"] integerValue];canvas.driver.height=[layout[@"height"] integerValue];
            canvas.driver.changed=^(NSString *message){
                if(message)dispatch_async(dispatch_get_main_queue(),^{[weakSelf fail:message];});
                else[weakSelf checkLoading];
            };
            view.delegate=canvas.driver;canvas.presentation=view;canvas.started=YES;
        } else {
            const FAHWebBackend backend=[_config->webBackend isEqual:@"legacy"]?FAHWebBackend::Legacy:FAHWebBackend::WK;
            const FAHWebClock clock=[_config->webClock isEqual:@"native"]?FAHWebClock::Native:FAHWebClock::AnimationFrame;
            NSError *error=nil;canvas.web=[[FAHWebRenderer alloc] initWithAssetsURL:_config->assets backend:backend clock:clock error:&error];
            require(canvas.web!=nil,"Web renderer could not be created");
            [canvas.web setOptions:_config->options];[canvas.web setCapture:_config->capture];
            canvas.presentation=[canvas.web prepareWithFrame:NSMakeRect(0,0,width,height)];require(canvas.presentation!=nil,"Web presentation view could not be created");
            __weak ECCanvas *weakCanvas=canvas;
            canvas.web.stateChanged=^{ECHarness *owner=weakSelf;ECCanvas *item=weakCanvas;if(!owner||!item)return;
                if(item.web.lastError.length)[owner fail:item.web.lastError];else[owner checkLoading];};
        }
        canvas.window.contentView=canvas.presentation;[_canvases addObject:canvas];
        if(!_config->hiddenReference)[canvas.window makeKeyAndOrderFront:nil];
    }
    if(!_config->hiddenReference)[NSApp activateIgnoringOtherApps:YES];
    if(([_config->engine isEqual:@"metal"]||[_config->webClock isEqual:@"native"]) && ![_config->mode isEqual:@"screenshot"])
        _drawTimer=[NSTimer scheduledTimerWithTimeInterval:1.0/_config->options.fps repeats:YES block:^(NSTimer *timer){(void)timer;[weakSelf drawClockFrame];}];
    _loadingTimer=[NSTimer scheduledTimerWithTimeInterval:.025 repeats:YES block:^(NSTimer *timer){(void)timer;[weakSelf checkLoading];}];
    [self checkLoading];
}
- (void)drawClockFrame {
    if(_finishing||_terminal)return;
    protect(self,^{const double timestamp=now();
        for(ECCanvas *canvas in self->_canvases) {
            if(canvas.metal)[(MTKView*)canvas.presentation draw];
            else if(canvas.started&&canvas.web.ready) {
                ++canvas.nativeClockRequests;
                if([canvas.web advanceTo:std::max(0.0,timestamp-canvas.clockEpoch)])++canvas.nativeClockAccepted;
            }
        }
    });
}
- (void)checkLoading {
    if(![_phase isEqual:@"loading"] || _finishing)return;
    protect(self, ^{
        BOOL all=YES;
        for(ECCanvas *canvas in self->_canvases) {
            if(canvas.web) {
                if(canvas.web.lastError.length){[self fail:canvas.web.lastError];return;}
                if(!canvas.web.ready || [canvas.web.metrics[@"pending_updates"] boolValue]) {all=NO;continue;}
                if(!canvas.started && [self->_config->mode isEqual:@"benchmark"]) {canvas.clockEpoch=now();[canvas.web start];canvas.started=YES;all=NO;continue;}
                if([self->_config->mode isEqual:@"benchmark"]&&[canvas.web.metrics[@"draws"] unsignedLongLongValue]==0){all=NO;continue;}
                if(!canvas.ready && !canvas.sizing) {
                    canvas.sizing=YES;__weak ECHarness *weakSelf=self;
                    NSString *sizeScript=[self->_config->mode isEqual:@"screenshot"]?
                        [NSString stringWithFormat:@"(()=>{window.fahSaver.renderAt(%.17g);return {width:document.querySelector('canvas').width,height:document.querySelector('canvas').height};})()",self->_config->fixedTime]:
                        @"({width:document.querySelector('canvas').width,height:document.querySelector('canvas').height})";
                    @autoreleasepool {[canvas.web evaluateRuntimeScript:sizeScript completion:^(id result,NSError *error){
                        @autoreleasepool { ECHarness *owner=weakSelf;if(!owner)return;protect(owner,^{require(!error&&[result isKindOfClass:NSDictionary.class],"Web canvas dimensions are unavailable");
                            canvas.actualWidth=[result[@"width"] integerValue];canvas.actualHeight=[result[@"height"] integerValue];
                            require(canvas.actualWidth==[canvas.layout[@"width"] integerValue]&&canvas.actualHeight==[canvas.layout[@"height"] integerValue],"Web canvas dimensions do not match the requested layout");canvas.ready=YES;[owner checkLoading];}); }
                    }];}
                }
                if(!canvas.ready)all=NO;
            } else {
                const CGSize size=((MTKView*)canvas.presentation).drawableSize;
                canvas.actualWidth=static_cast<NSInteger>(size.width);canvas.actualHeight=static_cast<NSInteger>(size.height);
                require(size.width==[canvas.layout[@"width"] integerValue]&&size.height==[canvas.layout[@"height"] integerValue],"Metal view dimensions do not match the requested layout");
                canvas.ready=[self->_config->mode isEqual:@"screenshot"]||canvas.driver.submissions>0;
                if(!canvas.ready)all=NO;
            }
        }
        if(!all||self->_canvases.count!=self->_config->layouts.count)return;
        [self->_loadingTimer invalidate];self->_loadingTimer=nil;[self->_diagnosticTimer invalidate];self->_diagnosticTimer=nil;self->_times[@"ready_uptime"]=@(now());
        if([self->_config->mode isEqual:@"screenshot"]){[self screenshot];return;}
        [self phase:@"warmup" timestamp:@"warmup_start_uptime"];
        __weak ECHarness *weakSelf=self;
        self->_phaseTimer=[NSTimer scheduledTimerWithTimeInterval:self->_config->warmup repeats:NO block:^(NSTimer *timer){(void)timer;[weakSelf beginMeasurement];}];
    });
}
- (void)beginMeasurement {
    if(_finishing)return;
    if(_config->options.collectMetrics&&[_config->engine isEqual:@"web"]) {
        for(ECCanvas *canvas in _canvases)[canvas.web beginMeasurement];
        __weak ECHarness *weakSelf=self;
        _collectionTimer=[NSTimer scheduledTimerWithTimeInterval:3 repeats:NO block:^(NSTimer *timer){(void)timer;[weakSelf fail:@"Timed out starting Web measurement"];}];
        _loadingTimer=[NSTimer scheduledTimerWithTimeInterval:.005 repeats:YES block:^(NSTimer *timer){(void)timer;ECHarness *owner=weakSelf;if(!owner)return;
            for(ECCanvas *canvas in owner->_canvases)if([canvas.web.metrics[@"pending_updates"] boolValue])return;
            [owner->_loadingTimer invalidate];owner->_loadingTimer=nil;[owner->_collectionTimer invalidate];owner->_collectionTimer=nil;[owner captureBaselines];}];
    } else [self captureBaselines];
}
- (void)captureBaselines {
    if(_finishing)return;__block NSUInteger remaining=_canvases.count;__weak ECHarness *weakSelf=self;
    for(ECCanvas *canvas in _canvases) {
        if(canvas.metal) {
            if(_config->options.collectMetrics)[canvas.metal beginMeasurement];
            canvas.baseline=canvas.metal.metrics;canvas.baselineDraws=canvas.driver.submissions;canvas.baselineUptime=now();
            if(--remaining==0)[self measurementReady];
        } else {
            @autoreleasepool {[canvas.web refreshMetrics:^(NSDictionary *metrics){@autoreleasepool {ECHarness *owner=weakSelf;if(!owner||owner->_finishing)return;protect(owner,^{canvas.baseline=metrics;canvas.baselineDraws=draws(metrics).unsignedLongLongValue;canvas.baselineUptime=now();canvas.baselineClockRequests=canvas.nativeClockRequests;canvas.baselineClockAccepted=canvas.nativeClockAccepted;if(--remaining==0)[owner measurementReady];});}}];}
        }
    }
}
- (void)measurementReady {
    protect(self,^{[self phase:@"measurement" timestamp:@"measurement_start_uptime"];__weak ECHarness *weakSelf=self;
        self->_phaseTimer=[NSTimer scheduledTimerWithTimeInterval:self->_config->measure repeats:NO block:^(NSTimer *timer){(void)timer;[weakSelf finishMeasurement];}];});
}
- (void)finishMeasurement {
    if(_finishing)return;_finishing=YES;_times[@"measurement_end_uptime"]=@(now());
    protect(self,^{[self phase:@"measurement" timestamp:nil];});
    if(_terminal)return;
    [_drawTimer invalidate];_drawTimer=nil;__weak ECHarness *weakSelf=self;
    _collectionTimer=[NSTimer scheduledTimerWithTimeInterval:3 repeats:NO block:^(NSTimer *timer){(void)timer;[weakSelf fail:@"Timed out collecting engine metrics"];}];
    if([_config->engine isEqual:@"web"]&&[_config->webClock isEqual:@"native"]) {
        _loadingTimer=[NSTimer scheduledTimerWithTimeInterval:.005 repeats:YES block:^(NSTimer *timer){(void)timer;ECHarness *owner=weakSelf;if(!owner)return;
            protect(owner,^{for(ECCanvas *canvas in owner->_canvases) {
                NSDictionary *state=canvas.web.metrics;
                require([state[@"pending_native_frame"] isKindOfClass:NSNumber.class],"Web native-clock pending state is unavailable");
                if([state[@"pending_updates"] boolValue]||[state[@"pending_native_frame"] boolValue])return;
            }
            [owner->_loadingTimer invalidate];owner->_loadingTimer=nil;[owner collectFinalMetrics];});}];
    } else [self collectFinalMetrics];
}
- (void)collectFinalMetrics {
    if(_terminal||_cancelled)return;
    __block NSUInteger remaining=_canvases.count;__weak ECHarness *weakSelf=self;
    for(ECCanvas *canvas in _canvases) {
        if(canvas.metal) {canvas.finalMetrics=canvas.metal.metrics;canvas.finalDraws=canvas.driver.submissions;canvas.finalUptime=now();if(--remaining==0)[self stopped];}
        else @autoreleasepool {[canvas.web evaluateRuntimeScript:@"(()=>{window.fahSaver.stop();return window.fahSaver.metrics();})()" completion:^(id result,NSError *error){@autoreleasepool {ECHarness *owner=weakSelf;if(!owner||owner->_terminal||owner->_cancelled)return;protect(owner,^{require(!error&&[result isKindOfClass:NSDictionary.class],"Web final metrics are unavailable");canvas.finalMetrics=result;canvas.finalDraws=draws(result).unsignedLongLongValue;canvas.finalUptime=now();canvas.finalClockRequests=canvas.nativeClockRequests;canvas.finalClockAccepted=canvas.nativeClockAccepted;if(--remaining==0)[owner stopped];});}}];}
    }
}
- (void)releasePresentation {
    @autoreleasepool {
        [_drawTimer invalidate];_drawTimer=nil;[_loadingTimer invalidate];_loadingTimer=nil;[_phaseTimer invalidate];_phaseTimer=nil;
        [_diagnosticTimer invalidate];_diagnosticTimer=nil;[_diagnosticDeadline invalidate];_diagnosticDeadline=nil;
        for(ECCanvas *canvas in _canvases) {
            canvas.web.stateChanged=nil;[canvas.web stop];[canvas.metal stop];
            if(canvas.metal&&_times[@"measurement_end_uptime"]&&!_cancelled&&!_failure)canvas.finalMetrics=canvas.metal.metrics;
            if(canvas.metal){((MTKView*)canvas.presentation).delegate=nil;canvas.driver.changed=nil;}
            canvas.window.contentView=[[NSView alloc]initWithFrame:NSMakeRect(0,0,1,1)];
            [canvas.presentation removeFromSuperview];canvas.presentation=nil;
            canvas.window.delegate=nil;canvas.window.escapeHandler=nil;[canvas.window orderOut:nil];[canvas.window close];canvas.window=nil;
        }
    }
}
- (void)stopped {
    if(_terminal||_cancelled)return;
    protect(self,^{[self->_collectionTimer invalidate];self->_collectionTimer=nil;[self releasePresentation];
        for(ECCanvas *canvas in self->_canvases) {
            NSString *error=canvas.metal?canvas.metal.lastError:canvas.web.lastError;
            if(error.length){[self fail:error];return;}
        }
        [self phase:@"stopped" timestamp:@"stopped_uptime"];__weak ECHarness *weakSelf=self;
        self->_phaseTimer=[NSTimer scheduledTimerWithTimeInterval:self->_config->cooldown repeats:NO block:^(NSTimer *timer){(void)timer;[weakSelf complete];}];});
}
- (NSDictionary *)report {
    NSMutableArray *views=[NSMutableArray new];
    for(ECCanvas *canvas in _canvases) {
        const double seconds=canvas.finalUptime-canvas.baselineUptime;
        const auto delta=canvas.finalDraws>=canvas.baselineDraws?canvas.finalDraws-canvas.baselineDraws:0;
        [views addObject:@{@"layout":canvas.layout,@"backing_scale":@(canvas.scale),@"actual_width":@(canvas.actualWidth),@"actual_height":@(canvas.actualHeight),@"baseline":canvas.baseline?:@{},@"final":canvas.finalMetrics?:@{},
            @"final_metrics_source":canvas.finalMetricsFromFailureCache?@"cached_before_failure":(canvas.finalMetrics?@"normal_collection":@"unavailable"),
            @"failure_state_before_release":canvas.failureState?:@{},
            @"native_clock_requests":@(canvas.nativeClockRequests),@"native_clock_accepted":@(canvas.nativeClockAccepted),
            @"measured_native_clock_requests":@(canvas.finalClockRequests>=canvas.baselineClockRequests?canvas.finalClockRequests-canvas.baselineClockRequests:0),
            @"measured_native_clock_accepted":@(canvas.finalClockAccepted>=canvas.baselineClockAccepted?canvas.finalClockAccepted-canvas.baselineClockAccepted:0),
            @"native_clock_counter_scope":@"Accepted means queued/coalesced JavaScript work, not completed GPU frames",
            @"draw_count":@(delta),@"draw_count_elapsed_seconds":@(std::max(0.0,seconds)),
            @"draw_count_rate":@(seconds>0?delta/seconds:0),
            @"cadence_scope":canvas.metal?@"Successful Metal command-buffer submissions; not display presentations":@"WebGL draw calls; not display presentations"}];
    }
    return @{@"harness":@"FAH matched windowed app comparison",@"pid":@(getpid()),@"bundle_id":NSBundle.mainBundle.bundleIdentifier?:@"",
        @"engine":_config->engine,@"web_backend":[_config->engine isEqual:@"web"]?_config->webBackend:@"not_applicable",
        @"web_clock":[_config->engine isEqual:@"web"]?_config->webClock:@"not_applicable",
        @"mode":_config->mode,@"execution_mode":_config->hiddenReference?@"hidden_reference_cli":@"windowed_app",
        @"capture_sha256":_config->captureHash,@"atoms":@(_config->capture->topology.atoms.size()),
        @"bonds":@(_config->capture->topology.bonds.size()),@"snapshots":@(_config->capture->frames.size()),@"style":@(static_cast<int>(_config->options.style)),
        @"fps_cap":@(_config->options.fps),@"background":_config->background,@"playback":_config->options.loop?@"loop":@"latest",
        @"snapshot_interval_seconds":@(_config->options.snapshotInterval),@"collect_metrics":@(_config->options.collectMetrics),
        @"gpu_timing_scope":!_config->options.collectMetrics?@"Disabled for normal-cost comparison":([_config->engine isEqual:@"metal"]?@"Metal command-buffer GPU timestamps":@"WebGL timer queries; inspect gpuTiming availability"),
        @"warmup_seconds":@(_config->warmup),@"measure_seconds":@(_config->measure),@"cooldown_seconds":@(_config->cooldown),
        @"phase_times":_times,@"views":views,@"loading_diagnostics":_loadingDiagnostics?:@[],@"cancelled":@(_cancelled),@"completion_reason":_reason?:@"bounded_duration",@"error":_failure?:@"",
        @"presentation_owners_released":@([_phase isEqual:@"stopped"]||[_phase isEqual:@"complete"]),
        @"scope":@"Fixture-only rendering; excludes live FAH, source networking and snapshot cache"};
}
- (void)complete {
    protect(self,^{if(!self->_cancelled&&[self->_config->mode isEqual:@"benchmark"])
        for(ECCanvas *canvas in self->_canvases)if(canvas.metal)require(canvas.driver.submissions==canvas.finalDraws,"Metal submissions continued after stop");
        self->_reason=self->_reason?:@"bounded_duration";[self phase:@"complete" timestamp:@"complete_uptime"];writeJSON([self report],self->_config->output);[self leave];});
}
- (void)leave {
    _terminal=YES;
    [_hardTimer invalidate];[_collectionTimer invalidate];[_phaseTimer invalidate];
    for(ECCanvas *canvas in _canvases){canvas.window.delegate=nil;canvas.window.escapeHandler=nil;[canvas.window close];}
    [NSApp stop:nil];[NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}
- (void)fail:(NSString *)message {
    if(_terminal||[_phase isEqual:@"error"])return;
    _finishing=YES;_exitCode=1;_failure=[message substringToIndex:std::min<NSUInteger>(message.length,1024)];
    for(ECCanvas *canvas in _canvases) {
        canvas.failureState=[self cachedState:canvas];
        if(!canvas.finalMetrics){canvas.finalMetrics=canvas.failureState[@"metrics"];canvas.finalMetricsFromFailureCache=YES;}
    }
    [self releasePresentation];
    try{[self phase:@"error" timestamp:@"error_uptime"];writeJSON([self report],_config->output);}catch(const std::exception&) {std::fprintf(stderr,"Could not write comparison failure report\n");}
    [self leave];
}
- (void)cancel:(NSString *)reason {if(_terminal)return;_cancelled=YES;_finishing=YES;_reason=reason;[_collectionTimer invalidate];[self releasePresentation];[self complete];}
- (BOOL)windowShouldClose:(NSWindow *)sender {(void)sender;[self cancel:@"window_closed"];return NO;}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {(void)sender;[self cancel:@"application_quit"];return NSTerminateCancel;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {(void)sender;return NO;}
- (void)screenshot {
    protect(self,^{ECCanvas *canvas=self->_canvases.firstObject;
        if(canvas.metal) {
            const NSUInteger width=[canvas.layout[@"width"] unsignedIntegerValue],height=[canvas.layout[@"height"] unsignedIntegerValue];
            MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
            descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageRenderTarget;
            id<MTLTexture> texture=[canvas.metal.device newTextureWithDescriptor:descriptor];require(texture!=nil,"Could not allocate screenshot texture");
            id<MTLCommandBuffer> command=[canvas.metal renderToTexture:texture time:0 drawable:nil];require(command!=nil,"Metal fixed render failed");[command waitUntilCompleted];
            if(self->_config->fixedTime>0){command=[canvas.metal renderToTexture:texture time:self->_config->fixedTime drawable:nil];require(command!=nil,"Metal fixed render failed");[command waitUntilCompleted];}
            require(command.status==MTLCommandBufferStatusCompleted,"Metal fixed render did not complete");
            NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:nil pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width*4 bitsPerPixel:32];
            require(bitmap!=nil,"Screenshot bitmap allocation failed");[texture getBytes:bitmap.bitmapData bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
            for(NSUInteger i=0;i<width*height;++i)std::swap(bitmap.bitmapData[4*i],bitmap.bitmapData[4*i+2]);
            NSData *png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];[self savePNG:png canvas:canvas];
        } else {
            NSString *script=[NSString stringWithFormat:@"(()=>{window.fahSaver.stop();window.fahSaver.renderAt(%.17g);return {metrics:window.fahSaver.metrics(),png:document.querySelector('canvas').toDataURL('image/png')};})()",self->_config->fixedTime];
            __weak ECHarness *weakSelf=self;
            @autoreleasepool {[canvas.web evaluateRuntimeScript:script completion:^(id result,NSError *error){@autoreleasepool {ECHarness *owner=weakSelf;if(!owner)return;protect(owner,^{require(!error&&[result isKindOfClass:NSDictionary.class],"Web fixed render failed");NSString *url=result[@"png"];require([url isKindOfClass:NSString.class]&&[url hasPrefix:@"data:image/png;base64,"],"Web canvas did not produce PNG data");canvas.finalMetrics=result[@"metrics"];NSData *png=[[NSData alloc]initWithBase64EncodedString:[url substringFromIndex:22] options:0];[owner savePNG:png canvas:canvas];});}}];}
        }
    });
}
- (void)savePNG:(NSData *)png canvas:(ECCanvas *)canvas {
    NSBitmapImageRep *bitmap=png?[NSBitmapImageRep imageRepWithData:png]:nil;
    require(bitmap&&bitmap.pixelsWide==[canvas.layout[@"width"] integerValue]&&bitmap.pixelsHigh==[canvas.layout[@"height"] integerValue],"Screenshot dimensions do not match the requested canvas");
    NSColor *first=[bitmap colorAtX:0 y:0];NSUInteger different=0;
    for(NSInteger y=0;y<bitmap.pixelsHigh;y+=std::max<NSInteger>(1,bitmap.pixelsHigh/64))
        for(NSInteger x=0;x<bitmap.pixelsWide;x+=std::max<NSInteger>(1,bitmap.pixelsWide/64))if(![[bitmap colorAtX:x y:y] isEqual:first])++different;
    require(different>16,"Screenshot canvas is blank or uniform");NSError *error=nil;
    require([png writeToFile:_config->screenshot options:NSDataWritingAtomic error:&error],"Could not write fixed-time PNG");
    if(canvas.metal)canvas.finalMetrics=canvas.metal.metrics;
    NSMutableDictionary *metrics=[canvas.finalMetrics mutableCopy]?:[NSMutableDictionary new];metrics[@"png_sha256"]=sha256(png);metrics[@"nonuniform_samples"]=@(different);metrics[@"fixed_time_seconds"]=@(_config->fixedTime);canvas.finalMetrics=metrics;
    _finishing=YES;[self stopped];
}
@end

static int selfTest() {
    NSString *directory=[NSTemporaryDirectory() stringByAppendingPathComponent:[@"fah-comparison-config-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
    @try {
        NSString *fixture=[directory stringByAppendingPathComponent:@"fixture.json"];
        [@"{\"topology\":{\"atoms\":[[\"C\"]],\"bonds\":[]},\"frames\":[[[0,0,0]]]}" writeToFile:fixture atomically:YES encoding:NSUTF8StringEncoding error:nil];
        NSMutableDictionary *base=[@{@"engine":@"metal",@"capture":fixture,@"output":[directory stringByAppendingPathComponent:@"out.json"],@"status":[directory stringByAppendingPathComponent:@"status.json"]} mutableCopy];
        ECConfig *valid=configuration(base);require(valid->layouts.count==1&&valid->options.fps==60&&valid->warmup==10&&valid->measure==30&&valid->cooldown==10&&[valid->webBackend isEqual:@"wk"]&&[valid->webClock isEqual:@"raf"],"Comparison defaults changed");
        for(NSString *backend in @[@"wk",@"legacy"]) {
            NSMutableDictionary *selected=[base mutableCopy];selected[@"webBackend"]=backend;
            require([configuration(selected)->webBackend isEqual:backend],"Explicit Web backend selection changed");
        }
        for(NSString *clock in @[@"raf",@"native"]) {
            NSMutableDictionary *selected=[base mutableCopy];selected[@"webClock"]=clock;
            require([configuration(selected)->webClock isEqual:clock],"Explicit Web clock selection changed");
        }
        bool rejectedReference=false;try{referenceMode(valid);}catch(const std::runtime_error&){rejectedReference=true;}
        require(rejectedReference,"Hidden reference mode accepted a benchmark configuration");
        NSMutableDictionary *reference=[base mutableCopy];reference[@"mode"]=@"screenshot";reference[@"screenshot"]=[directory stringByAppendingPathComponent:@"reference.png"];
        ECConfig *referenceConfig=configuration(reference);referenceMode(referenceConfig);
        require(referenceConfig->hiddenReference,"Screenshot reference mode was not applied");
        for(NSDictionary *change in @[@{@"engine":@"browser"},@{@"webBackend":@"auto"},@{@"webBackend":@1},@{@"webClock":@"auto"},@{@"webClock":@1},@{@"fps":@60.5},@{@"style":@4},@{@"layouts":@[]},@{@"measureSeconds":@0},@{@"warmupSeconds":@(-1)},@{@"cooldownSeconds":@(-1)},@{@"collectMetrics":@1},@{@"background":@"other"},@{@"snapshotIntervalSeconds":@0},@{@"output":fixture},@{@"width":@4097},@{@"unexpected":@1}]) {
            NSMutableDictionary *bad=[base mutableCopy];[bad addEntriesFromDictionary:change];bool failed=false;try{configuration(bad);}catch(const std::runtime_error&){failed=true;}require(failed,"Invalid comparison config was accepted");
        }
        [base addEntriesFromDictionary:@{@"layouts":@[@{@"width":@2880,@"height":@1800,@"screen":@0},@{@"width":@1920,@"height":@1080,@"screen":@1}],@"playback":@"loop",@"collectMetrics":@YES}];
        require(configuration(base)->layouts.count==2,"Matched multi-view configuration failed");
        std::puts("Engine comparison configuration checks passed");
    } @finally {[NSFileManager.defaultManager removeItemAtPath:directory error:nil];}
    return 0;
}
int main(int argc,const char *argv[]) {
    @autoreleasepool {
        try {
            if(argc==2&&std::string(argv[1])=="--help") {std::puts("FAHEngineComparison.app reads Resources/Comparison.json when launched normally.\n--validate-config ABS_JSON validates without creating UI.\n--self-test-config runs bounded configuration checks without creating UI.\n--render-reference ABS_JSON requires screenshot mode; renders using unshown views without ordering or activating windows. This is not a responsibility/performance comparison.");return 0;}
            if(argc==2&&std::string(argv[1])=="--self-test-config")return selfTest();
            if(argc==3&&std::string(argv[1])=="--validate-config") {ECConfig *config=readConfiguration([NSString stringWithUTF8String:argv[2]]);std::printf("Comparison config valid: engine=%s views=%lu atoms=%lu frames=%lu\n",config->engine.UTF8String,(unsigned long)config->layouts.count,(unsigned long)config->capture->topology.atoms.size(),(unsigned long)config->capture->frames.size());return 0;}
            const bool hidden=argc==3&&std::string(argv[1])=="--render-reference";
            require(argc==1||hidden,"Use the bundled Comparison.json or a documented option");
            ECConfig *config;@autoreleasepool{config=readConfiguration(hidden?[NSString stringWithUTF8String:argv[2]]:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Comparison.json"]);if(hidden)referenceMode(config);}
            [NSApplication sharedApplication];[NSApp setActivationPolicy:hidden?NSApplicationActivationPolicyProhibited:NSApplicationActivationPolicyRegular];
            if(!hidden){NSMenu *menu=[NSMenu new];NSMenuItem *appItem=[NSMenuItem new];[menu addItem:appItem];NSMenu *appMenu=[NSMenu new];[appMenu addItemWithTitle:@"Quit Engine Comparison" action:@selector(terminate:) keyEquivalent:@"q"];appItem.submenu=appMenu;NSApp.mainMenu=menu;}
            ECHarness *owner=[[ECHarness alloc]initWithConfig:config];NSApp.delegate=owner;[NSApp run];return owner.exitCode;
        } catch(const std::exception& error){std::fprintf(stderr,"EngineComparison: %s\n",error.what());return 2;}
    }
}
