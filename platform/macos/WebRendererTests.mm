#import "FAHWebRenderer.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>

static void check(bool value,const char *message) {
    if (!value) throw std::runtime_error(message);
}
static void waitFor(const std::function<bool()>& condition,const char *message) {
    const double deadline = NSProcessInfo.processInfo.systemUptime+20;
    while (!condition() && NSProcessInfo.processInfo.systemUptime < deadline)
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    check(condition(),message);
}
static void ready(FAHWebRenderer *renderer) {
    waitFor([&]{return renderer.lastError || (renderer.ready && ![[renderer metrics][@"pending_updates"] boolValue]);},
            "Web renderer did not finish its pending updates");
    if (renderer.lastError) throw std::runtime_error(renderer.lastError.UTF8String);
}
static NSDictionary *evaluate(FAHWebRenderer *renderer,NSString *script) { @autoreleasepool {
    auto done = std::make_shared<bool>(false);
    __block id result;
    __block NSError *failure;
    [renderer.webView evaluateJavaScript:script completionHandler:^(id value,NSError *error) {
        result = value; failure = error; *done = true;
    }];
    waitFor([&]{return *done;},"Web renderer evaluation timed out");
    if (failure) throw std::runtime_error(failure.localizedDescription.UTF8String);
    check([result isKindOfClass:NSDictionary.class],"Web renderer returned invalid metrics");
    return result;
}}
static NSDictionary *draw(FAHWebRenderer *renderer,double seconds) {
    return evaluate(renderer,[NSString stringWithFormat:@"window.fahSaver.renderAt(%.17g);window.fahSaver.metrics()",seconds]);
}
int main(int argc,const char *argv[]) {
    @autoreleasepool {
        try {
            check(argc == 3,"Supply Web renderer assets and a capture fixture");
            [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
            auto capture = std::make_shared<const fah::Capture>(fah::readCapture(argv[2]));
            check(capture->frames.size() == 2,"Web timing fixture must contain two frames");
            NSError *error = nil;
            FAHWebRenderer *renderer = [[FAHWebRenderer alloc] initWithAssetsURL:[NSURL fileURLWithPath:
                [NSString stringWithUTF8String:argv[1]] isDirectory:YES] error:&error];
            check(renderer != nil,"Could not create the Web renderer");
            NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,64,64)
                styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0,0,64,64)]; window.contentView = host;
            @autoreleasepool { [host addSubview:[renderer prepareWithFrame:host.bounds]]; }
            [renderer setCapture:capture];
            FAHRenderOptions options; options.loop = true;
            [renderer setOptions:options]; [renderer start]; ready(renderer);
            for (double interval : {1.0,1.25,120.0}) {
                options.snapshotInterval = interval; [renderer setOptions:options]; ready(renderer);
                for (const auto sample : {std::pair<double,unsigned>{0,0},{interval-.001,0},{interval,1},{2*interval,0}})
                    check([draw(renderer,sample.first)[@"snapshotIndex"] unsignedIntegerValue] == sample.second,
                          "Web snapshot loop advanced at the wrong boundary");
            }
            options.loop = false; [renderer setOptions:options]; ready(renderer);
            check([draw(renderer,0)[@"snapshotIndex"] unsignedIntegerValue] == 0,
                  "Latest playback did not keep its one-frame model");
            auto streaming = std::make_shared<fah::Capture>(*capture);
            streaming->frames.resize(1);
            [renderer setCapture:streaming]; ready(renderer);
            evaluate(renderer,@"(()=>{window.bridgeCalls={starts:0,replacements:0,initialFrames:[],appendSizes:[]};for(const name of ['start','replaceLatest','setCapture','appendFrames']){const original=window.fahSaver[name];window.fahSaver[name]=function(value){if(name==='start')bridgeCalls.starts++;if(name==='replaceLatest')bridgeCalls.replacements++;if(name==='setCapture')bridgeCalls.initialFrames.push(value?.frames.length||0);if(name==='appendFrames')bridgeCalls.appendSizes.push(value.length);return original(value);};}return bridgeCalls;})()");
            for (unsigned update=0; update<100; ++update) {
                streaming->frames.front().front().x += .001;
                [renderer setCapture:streaming];
            }
            ready(renderer);
            NSDictionary *calls = evaluate(renderer,@"window.bridgeCalls");
            check([calls[@"replacements"] unsignedIntegerValue] <= 2 && [calls[@"starts"] unsignedIntegerValue] == 0,
                  "Latest updates queued unbounded IPC or restarted the rotation clock");
            check([evaluate(renderer,@"window.fahSaver.metrics()")[@"snapshotCount"] unsignedIntegerValue] == 1,
                  "Latest replacement accumulated a trajectory");
            options.loop = true; [renderer setOptions:options]; ready(renderer);
            streaming->frames.push_back(capture->frames.back());
            [renderer setCapture:streaming]; ready(renderer);
            check([evaluate(renderer,@"window.fahSaver.metrics()")[@"snapshotCount"] unsignedIntegerValue] == 2,
                  "Loop frame was not appended exactly once");
            auto history = std::make_shared<fah::Capture>(*capture);
            history->frames.resize(128,capture->frames.front());
            evaluate(renderer,@"window.bridgeCalls.initialFrames=[];window.bridgeCalls.appendSizes=[];window.bridgeCalls");
            [renderer setCapture:history]; ready(renderer);
            calls = evaluate(renderer,@"window.bridgeCalls");
            NSArray *initialFrames = calls[@"initialFrames"], *appendSizes = calls[@"appendSizes"];
            NSUInteger appended = 0;
            for (NSNumber *count in appendSizes) {
                check(count.unsignedIntegerValue <= 16,"Loop bridge sent an unbounded frame batch");
                appended += count.unsignedIntegerValue;
            }
            check(initialFrames.count == 1 && [initialFrames[0] unsignedIntegerValue] == 1 && appended == 127,
                  "Loop bridge resent full history or lost streamed frames");
            options.styleMode = FAHStyleMode::Cycle;
            [renderer setOptions:options]; ready(renderer);
            check([draw(renderer,10)[@"style"] unsignedIntegerValue] == 2 &&
                  [draw(renderer,20)[@"style"] unsignedIntegerValue] == 3,
                  "Web style cycle did not reach all three model styles");
            options.styleMode = FAHStyleMode::Fixed; options.sky = true; options.darkSky = false;
            [renderer setOptions:options]; ready(renderer);
            NSString *pixelScript = @"window.fahSaver.renderAt(0);(()=>{const g=document.querySelector('canvas').getContext('webgl2');const p=new Uint8Array(4);g.readPixels(0,0,1,1,g.RGBA,g.UNSIGNED_BYTE,p);return {pixel:Array.from(p)};})()";
            NSArray *light = evaluate(renderer,pixelScript)[@"pixel"];
            options.darkSky = true; [renderer setOptions:options]; ready(renderer);
            NSArray *dark = evaluate(renderer,pixelScript)[@"pixel"];
            for (unsigned channel=0; channel<3; ++channel)
                check([dark[channel] intValue] < [light[channel] intValue],"Web dark background did not change the pixels");
            options.sky = false; options.colour = {.2f,.3f,.4f};
            [renderer setOptions:options]; ready(renderer);
            NSDictionary *pixel = evaluate(renderer,@"window.fahSaver.renderAt(0);(()=>{const c=document.querySelector('canvas');const g=c.getContext('webgl2');const p=new Uint8Array(4);g.readPixels(0,0,1,1,g.RGBA,g.UNSIGNED_BYTE,p);return {pixel:Array.from(p),metrics:window.fahSaver.metrics()};})()");
            NSArray *rgba = pixel[@"pixel"];
            check(rgba.count == 4 && std::abs([rgba[0] intValue]-51) <= 1 &&
                  std::abs([rgba[1] intValue]-77) <= 1 && std::abs([rgba[2] intValue]-102) <= 1,
                  "Web renderer did not draw the configured background");
            [renderer setCapture:nullptr]; ready(renderer);
            check([evaluate(renderer,@"window.fahSaver.metrics()")[@"snapshotCount"] unsignedIntegerValue] == 0,
                  "Cleared source retained the previous model in WebKit");
            __weak WKWebView *old;
            @autoreleasepool { old = renderer.webView; [renderer stop]; }
            waitFor([&]{return old == nil;},"Stop retained the old WebView");
            check(!renderer.webView && !renderer.ready,"Stopped renderer retains its page");
            check(!window.visible,"Web test displayed a window");
            for (double interval : {0.0,.049,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
                options.snapshotInterval = interval; bool rejected = false;
                try { [renderer setOptions:options]; } catch (const std::exception&) { rejected = true; }
                check(rejected,"Web renderer accepted invalid snapshot timing");
            }
            options.snapshotInterval = 1e100; [renderer setOptions:options];
            std::puts("WebKit renderer passed: real WebGL draw, loop cadence, latest replacement, append, background, source clear and disposal");
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"Web renderer tests: %s\n",exception.what()); return 1;
        }
    }
}
