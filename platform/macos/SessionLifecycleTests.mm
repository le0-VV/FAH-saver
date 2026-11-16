#import <AppKit/AppKit.h>
#include "FAHSaverView.h"
#include "TestSource.hpp"

#include <cstdio>
#include <stdexcept>

@interface FAHSaverView (SessionFixture)
- (void)workspaceApplicationActivated:(NSNotification *)notification;
@end

@interface FixtureApplication : NSObject
@property(nonatomic, copy) NSString *bundleIdentifier;
@end
@implementation FixtureApplication
@end

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static void activation(FAHSaverView *view, BOOL login) {
    FixtureApplication *application = [FixtureApplication new];
    application.bundleIdentifier = login ? @"com.apple.loginwindow" : @"org.fahsaver.fixture-desktop";
    [view workspaceApplicationActivated:[NSNotification notificationWithName:NSWorkspaceDidActivateApplicationNotification
        object:nil userInfo:@{NSWorkspaceApplicationKey:application}]];
}

static void stopped(FAHSaverView *view) {
    check(!view.isAnimating && !view.hasActiveSource,"Dismissed session remains active");
    const auto animation = view.animationCallbackCount, source = view.sourceCallbackCount;
    [view animateOneFrame];
    [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.04]];
    check(animation == view.animationCallbackCount && source == view.sourceCallbackCount,
          "Dismissed session continues callbacks");
    check(![[view renderMetrics][@"cache_worker_active"] boolValue],"Dismissed session retains its cache worker");
    for (NSView *child in view.subviews)
        check(![child isKindOfClass:WKWebView.class] && ![child isKindOfClass:NSClassFromString(@"WebView")],"Dismissed session retains its presentation owner");
}

int main(int argc,const char *argv[]) {
    @autoreleasepool {
        try {
            check(argc == 3,"Supply saver bundle and capture fixture");
            [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
            NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:argv[1]]];
            NSError *error = nil;
            check(bundle && [bundle loadAndReturnError:&error],"Could not load saver bundle");
            auto fixture = std::make_shared<const fah::Capture>(fah::readCapture(argv[2]));
            TemporaryModelCache cache;
            NSUserDefaults *preferences = [[NSUserDefaults alloc] initWithSuiteName:
                [@"org.fahsaver.session-test." stringByAppendingString:NSUUID.UUID.UUIDString]];
            auto firstCount = std::make_shared<FixtureSourceCounters>();
            auto secondCount = std::make_shared<FixtureSourceCounters>();
            auto previewCount = std::make_shared<FixtureSourceCounters>();
            Class principal = bundle.principalClass;
            FAHSaverView *first = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:NO
                preferences:preferences cachePath:cache.file("first.cache") sourceFactory:fixtureSource(fixture,firstCount)];
            FAHSaverView *second = [[principal alloc] initWithFrame:NSMakeRect(0,0,240,320) isPreview:NO
                preferences:preferences cachePath:cache.file("second.cache") sourceFactory:fixtureSource(fixture,secondCount)];
            FAHSaverView *preview = [[principal alloc] initWithFrame:NSMakeRect(0,0,160,100) isPreview:YES
                preferences:preferences cachePath:cache.file("preview.cache") sourceFactory:fixtureSource(fixture,previewCount)];
            // Enable the private host guard in this fixture process without observing real workspace events.
            for (FAHSaverView *view in @[first,second,preview]) [view setValue:@YES forKey:@"sessionGuardEnabled"];
            for (FAHSaverView *view in @[first,second,preview]) { activation(view,NO); [view startAnimation]; }
            check(first.isAnimating && second.isAnimating && preview.isAnimating,
                  "Cold non-login/windowless startup was blocked");
            activation(first,NO);
            check(first.isAnimating,"Unarmed non-login activation stopped a cold start");
            for (FAHSaverView *view in @[first,second,preview]) { activation(view,YES); activation(view,YES); }
            check(firstCount->starts == 1 && secondCount->starts == 1,"Arming restarted an active display");
            for (FAHSaverView *view in @[first,second,preview]) activation(view,NO);
            stopped(first); stopped(second);
            check(preview.isAnimating && preview.hasActiveSource && previewCount->starts == 1,
                  "Session guard stopped a genuine preview");
            activation(first,NO);
            check(firstCount->stops == 1,"Duplicate workspace exit stopped a source twice");
            [first startAnimation]; [first startAnimation]; stopped(first);
            check(firstCount->starts == 1,"Stale inactive start bypassed the dismissal latch");
            activation(first,YES); activation(second,YES);
            check(first.isAnimating && firstCount->starts == 2 && !second.isAnimating && secondCount->starts == 1,
                  "New session revived an old display without a fresh host request");
            activation(second,NO); [second startAnimation]; stopped(second);
            check(secondCount->starts == 1,"Unrequested login cycle cleared an old display's dismissal latch");
            [second stopAnimation]; activation(second,YES); activation(second,NO);
            [second startAnimation]; stopped(second);
            check(secondCount->starts == 1,"Cancelled request plus an unrequested login cycle bypassed the latch");
            activation(second,YES);
            check(second.isAnimating && secondCount->starts == 2,"Fresh second-display host request did not resume at activation");
            activation(first,NO); activation(second,NO);
            [first startAnimation]; [first stopAnimation];
            [second startAnimation];
            activation(first,YES); activation(second,YES);
            stopped(first);
            check(firstCount->starts == 2 && second.isAnimating && secondCount->starts == 3,
                  "Explicit stop failed to cancel queued session resume");
            activation(second,NO); [second startAnimation]; second.hidden = YES;
            activation(second,YES);
            check(!second.isAnimating,"Session restart ran while its view was hidden");
            [second stopAnimation]; second.hidden = NO; stopped(second);
            [first startAnimation]; [first stopAnimation]; activation(first,NO); activation(first,YES); stopped(first);
            check(firstCount->starts == 3,"Explicit stopped view restarted on a later login activation");
            auto coldCount = std::make_shared<FixtureSourceCounters>();
            FAHSaverView *cold = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:NO
                preferences:preferences cachePath:cache.file("cold.cache") sourceFactory:fixtureSource(fixture,coldCount)];
            [cold setValue:@YES forKey:@"sessionGuardEnabled"]; activation(cold,NO);
            [cold startAnimation]; [cold stopAnimation]; [cold startAnimation]; stopped(cold);
            check(coldCount->starts == 1,"Ordinary explicit stop allowed a stale inactive restart");
            [cold stopAnimation]; activation(cold,YES); stopped(cold);
            [preview stopAnimation]; stopped(preview);
            std::puts("Session lifecycle passed: cold/windowless start, arm/exit, stale-start latch, fresh multi-display requests, explicit-stop precedence and preview independence");
            return 0;
        } catch(const std::exception &error) {
            std::fprintf(stderr,"Session lifecycle: %s\n",error.what()); return 1;
        }
    }
}
