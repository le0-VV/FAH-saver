#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "FAHSaverView.h"
#include "TestSource.hpp"

#include <cstdio>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static void drain() { @autoreleasepool {
    [CATransaction flush];
    [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.06]];
}}

static NSView *presentationView(FAHSaverView *view) {
    NSView *found = nil;
    for (NSView *child in view.subviews)
        if (([child isKindOfClass:WKWebView.class] || [child isKindOfClass:NSClassFromString(@"WebView")])) {
            check(found == nil,"A saver owns more than one presentation view");
            found = child;
        }
    return found;
}

static void expectStopped(FAHSaverView *view) {
    check(!view.isAnimating && !view.hasActiveSource,"Hidden/detached saver remains active");
    check(!presentationView(view),"Stopped saver retains its WebKit presentation owner");
    check(![[view renderMetrics][@"cache_worker_active"] boolValue],"Stopped saver retains its cache worker");
    const auto animation = view.animationCallbackCount, source = view.sourceCallbackCount;
    NSNumber *submitted = [view renderMetrics][@"submitted_frames"];
    [view animateOneFrame]; drain();
    check(view.animationCallbackCount == animation && view.sourceCallbackCount == source &&
          [[view renderMetrics][@"submitted_frames"] isEqual:submitted],"Callbacks or submissions continued while suspended");
}

struct Preferences {
    __strong NSString *name = [@"org.fahsaver.presentation-test." stringByAppendingString:NSUUID.UUID.UUIDString];
    __strong NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:name];
    ~Preferences() { [defaults removePersistentDomainForName:name]; }
};

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            check(argc == 3,"Supply saver bundle and capture fixture");
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
            NSError *error = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:argv[1]]];
            check(bundle && [bundle loadAndReturnError:&error],"Could not load saver bundle");
            Class principal = bundle.principalClass;
            auto fixture = std::make_shared<const fah::Capture>(fah::readCapture(argv[2]));
            TemporaryModelCache cache;
            Preferences preferences;
            auto counters = std::make_shared<FixtureSourceCounters>();
            FAHSaverView *view = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:NO
                preferences:preferences.defaults cachePath:cache.file() sourceFactory:fixtureSource(fixture,counters)];
            __weak FAHSaverView *weakView = view;
            counters->beforeStart = [weakView,counters] {
                if (counters->starts > 1)
                    check([[weakView renderMetrics][@"model_available"] boolValue] &&
                          [[weakView renderMetrics][@"using_cached_model"] boolValue],
                          "Resumed source started before its cached model was ready");
            };
            check(!view.window && !view.superview,"Initial view was unexpectedly hosted");
            [view startAnimation]; [view startAnimation]; drain();
            check(view.isAnimating && view.hasActiveSource && counters->starts == 1 && presentationView(view),
                  "Initial windowless start was blocked or started twice");
            [view stopAnimation]; expectStopped(view);
            auto hiddenCounters = std::make_shared<FixtureSourceCounters>();
            FAHSaverView *initiallyHidden = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:NO
                preferences:preferences.defaults cachePath:cache.file("hidden.cache") sourceFactory:fixtureSource(fixture,hiddenCounters)];
            initiallyHidden.hidden = YES; [initiallyHidden startAnimation];
            check(!initiallyHidden.isAnimating && !initiallyHidden.hasActiveSource && hiddenCounters->starts == 0,
                  "Start request ran while the view was hidden");
            initiallyHidden.hidden = NO; drain();
            check(initiallyHidden.isAnimating && initiallyHidden.hasActiveSource && hiddenCounters->starts == 1,
                  "Deferred hidden start did not resume on show");
            initiallyHidden.hidden = YES; [initiallyHidden stopAnimation]; [initiallyHidden startAnimation];
            [initiallyHidden stopAnimation]; initiallyHidden.hidden = NO; drain();
            check(!initiallyHidden.isAnimating && !initiallyHidden.hasActiveSource && hiddenCounters->starts == 1,
                  "Explicit stop did not cancel a deferred hidden start");
            NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,240)
                styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
            window.releasedWhenClosed = NO;
            NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0,0,320,240)];
            window.contentView = host; [host addSubview:view];
            for (unsigned cycle=0; cycle<20; ++cycle) {
                @autoreleasepool { [view startAnimation]; drain(); }
                const unsigned starts = counters->starts, stops = counters->stops;
                NSDictionary *running = [view renderMetrics];
                __weak NSView *oldView;
                __weak CALayer *oldLayer;
                @autoreleasepool {
                    NSView *web = presentationView(view);
                    check(web != nil,"Running saver lacks its presentation view");
                    oldView = web; oldLayer = web.layer;
                    if (cycle%3 == 0) view.hidden = YES;
                    else if (cycle%3 == 1) host.hidden = YES;
                    else [view removeFromSuperview];
                }
                expectStopped(view); drain();
                check(!oldView,"Suspended saver retains an old WebKit view after draining");
                check(!oldLayer,"Suspended saver retains an old WebKit layer after draining");
                check(counters->stops == stops+1,"Suspension did not stop its source exactly once");
                check([[view renderMetrics][@"drawable_width"] isEqual:running[@"drawable_width"]] &&
                      [[view renderMetrics][@"drawable_height"] isEqual:running[@"drawable_height"]],
                      "Stopped metrics lost the last drawable dimensions");
                @autoreleasepool {
                    if (cycle%3 == 0) view.hidden = NO;
                    else if (cycle%3 == 1) host.hidden = NO;
                    else {
                        view.hidden = NO; [view startAnimation];
                        check(counters->starts == starts,"Detached saver resumed before being reattached");
                        [host addSubview:view];
                    }
                    [view startAnimation]; drain();
                    check(view.isAnimating && view.hasActiveSource && presentationView(view) && counters->starts == starts+1,
                          "Presentation resume did not restart exactly once");
                }
            }
            view.hidden = YES; expectStopped(view);
            [view stopAnimation];
            const auto stopped = counters->starts;
            view.hidden = NO; drain();
            check(counters->starts == stopped && !view.isAnimating,"Explicit stop while hidden retained automatic resume intent");
            [view startAnimation]; [view removeFromSuperview]; expectStopped(view);
            [view stopAnimation]; [host addSubview:view]; drain();
            check(!view.isAnimating && !view.hasActiveSource,"Explicit stop while detached resumed on reattachment");
            [view startAnimation]; [view removeFromSuperview];
            host.hidden = YES; [host addSubview:view]; expectStopped(view);
            host.hidden = NO; drain();
            check(view.isAnimating && view.hasActiveSource,"Reattachment to a hidden parent lost its later resume");
            [view stopAnimation]; expectStopped(view);
            check(!window.isVisible,"Presentation test ordered a window on screen");
            [view startAnimation];
            [window close]; expectStopped(view);
            [view startAnimation]; [view stopAnimation]; expectStopped(view);
            counters->beforeStart = {};
            std::puts("Presentation lifecycle passed: initial windowless start, 20 retained-view hide/detach/resume cycles, released WebKit owners, stopped callbacks and explicit-stop precedence");
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"Presentation lifecycle: %s\n",exception.what());
            return 1;
        }
    }
}
