#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <ScreenSaver/ScreenSaver.h>
#import "FAHSaverView.h"

#include <cstdio>
#include <stdexcept>
#include <string>

static NSString * const ModuleName = @"org.fahsaver.screensaver";

@interface FAHSmokeWindow : NSWindow
@property(nonatomic, copy) void (^inputHandler)(NSString *reason);
@property(nonatomic) double acceptInputAfter;
@end
@implementation FAHSmokeWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
- (void)sendEvent:(NSEvent *)event {
    if (CACurrentMediaTime() >= self.acceptInputAfter) {
        switch (event.type) {
            case NSEventTypeKeyDown:
                if (self.inputHandler) self.inputHandler(@"key_input"); return;
            case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown:
            case NSEventTypeOtherMouseDown: case NSEventTypeMouseMoved:
            case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged: case NSEventTypeScrollWheel:
                if (self.inputHandler) self.inputHandler(@"mouse_input"); return;
            default: break;
        }
    }
    [super sendEvent:event];
}
@end

struct VolatilePreferences {
    __strong NSUserDefaults *defaults;
    __strong NSDictionary *previous;
    VolatilePreferences(NSUserDefaults *value, NSString *path) : defaults(value) {
        previous = [defaults volatileDomainForName:NSArgumentDomain];
        NSMutableDictionary *arguments = previous ? [previous mutableCopy] : [NSMutableDictionary new];
        arguments[@"inputMode"] = @1;
        arguments[@"capturePath"] = path;
        arguments[@"fps"] = @60;
        arguments[@"style"] = @1;
        arguments[@"loop"] = @NO;
        arguments[@"sky"] = @YES;
        [defaults setVolatileDomain:arguments forName:NSArgumentDomain];
    }
    ~VolatilePreferences() {
        if (previous) [defaults setVolatileDomain:previous forName:NSArgumentDomain];
        else [defaults removeVolatileDomainForName:NSArgumentDomain];
    }
};

static NSString *captureWindow(NSWindow *window, NSString *path) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/screencapture"];
    task.arguments = @[@"-x", [NSString stringWithFormat:@"-l%ld", (long)window.windowNumber], path];
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) return error.localizedDescription;
    const double deadline = CACurrentMediaTime()+1;
    while (task.running && CACurrentMediaTime() < deadline) [NSThread sleepForTimeInterval:.01];
    if (task.running) { [task terminate]; return @"Window capture timed out."; }
    if (task.terminationStatus != 0 || ![[NSFileManager defaultManager] fileExistsAtPath:path])
        return @"Window capture unavailable in this execution context.";
    return nil;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            if (argc < 3) throw std::runtime_error(
                "Usage: fah_macos_saver_smoke BUNDLE.saver CAPTURE.json [--fullscreen] [--test-input] [--screenshot PATH]");
            bool fullscreen = false, testInput = false;
            NSString *screenshot = nil;
            for (int i=3; i<argc; ++i) {
                const std::string option = argv[i];
                if (option == "--fullscreen") fullscreen = true;
                else if (option == "--test-input") testInput = true;
                else if (option == "--screenshot" && i+1 < argc)
                    screenshot = [NSString stringWithUTF8String:argv[++i]];
                else throw std::runtime_error("Unknown or incomplete smoke option: "+option);
            }
            if (!fullscreen && (testInput || screenshot))
                throw std::runtime_error("--test-input and --screenshot require --fullscreen");
            NSString *capturePath = [[NSString stringWithUTF8String:argv[2]] stringByStandardizingPath];
            fah::readCapture(capturePath.fileSystemRepresentation);
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            NSString *path = [[NSString stringWithUTF8String:argv[1]] stringByStandardizingPath];
            NSBundle *bundle = [NSBundle bundleWithPath:path];
            NSError *error = nil;
            if (!bundle || ![bundle loadAndReturnError:&error])
                throw std::runtime_error(error ? error.localizedDescription.UTF8String : "Could not load saver bundle");
            Class principal = bundle.principalClass;
            if (!principal || ![principal isSubclassOfClass:ScreenSaverView.class])
                throw std::runtime_error("Bundle principal class is not a ScreenSaverView");
            const NSRect frame = fullscreen ? NSScreen.mainScreen.frame : NSMakeRect(0,0,320,240);
            NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:ModuleName];
            NSDictionary *persistentBefore = [defaults persistentDomainForName:ModuleName];
            VolatilePreferences preferences(defaults,capturePath);
            FAHSaverView *view = [[principal alloc] initWithFrame:NSMakeRect(0,0,frame.size.width,frame.size.height)
                isPreview:!fullscreen preferences:defaults];
            if (!view || !view.hasConfigureSheet || !view.configureSheet)
                throw std::runtime_error("Saver view or configuration sheet could not be created");
            if ([defaults integerForKey:@"inputMode"] != 1)
                throw std::runtime_error("Volatile capture preferences were not applied");
            NSString *exitReason = @"ten_start_stop_cycles";
            NSString *screenshotError = nil;
            double elapsed = 0;
            NSUInteger frameCallbacks = 0;
            if (!fullscreen) {
                for (int cycle=0; cycle<10; ++cycle) {
                    [view startAnimation];
                    if (view.hasActiveSource) throw std::runtime_error("Capture test unexpectedly started a network source");
                    for (int index=0; index<3; ++index) { [view animateOneFrame]; ++frameCallbacks; }
                    [view stopAnimation];
                    if (view.isAnimating || view.hasActiveSource) throw std::runtime_error("Saver did not stop");
                    [view animateOneFrame];
                }
            } else {
                FAHSmokeWindow *window = [[FAHSmokeWindow alloc] initWithContentRect:frame
                    styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                window.releasedWhenClosed = NO;
                window.level = NSScreenSaverWindowLevel;
                window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
                window.acceptsMouseMovedEvents = YES;
                window.contentView = view;
                const auto previousPresentation = NSApp.presentationOptions;
                __block NSString *reason = nil;
                __block NSString *imageError = nil;
                __block NSUInteger callbacks = 0;
                __block NSTimer *frameTimer = nil, *exitTimer = nil, *inputTimer = nil, *screenshotTimer = nil;
                void (^finish)(NSString *) = ^(NSString *why) {
                    if (reason) return;
                    reason = why;
                    [frameTimer invalidate]; frameTimer = nil;
                    [exitTimer invalidate]; exitTimer = nil;
                    [inputTimer invalidate]; inputTimer = nil;
                    [screenshotTimer invalidate]; screenshotTimer = nil;
                    [view stopAnimation];
                    [window orderOut:nil];
                    [NSApp stop:nil];
                    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
                };
                window.inputHandler = finish;
                window.acceptInputAfter = CACurrentMediaTime()+.4;
                id observer = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidResignActiveNotification
                    object:NSApp queue:nil usingBlock:^(NSNotification * __unused note) { finish(@"application_deactivated"); }];
                [view startAnimation];
                if (view.hasActiveSource) throw std::runtime_error("Capture test unexpectedly started a network source");
                frameTimer = [NSTimer timerWithTimeInterval:view.animationTimeInterval repeats:YES block:^(NSTimer * __unused timer) {
                    [view animateOneFrame]; ++callbacks;
                }];
                frameTimer.tolerance = .001;
                [NSRunLoop.mainRunLoop addTimer:frameTimer forMode:NSRunLoopCommonModes];
                exitTimer = [NSTimer scheduledTimerWithTimeInterval:5 repeats:NO block:^(NSTimer * __unused timer) { finish(@"five_second_autoexit"); }];
                if (testInput) inputTimer = [NSTimer scheduledTimerWithTimeInterval:1.5 repeats:NO block:^(NSTimer * __unused timer) {
                    NSEvent *event = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
                        timestamp:CACurrentMediaTime() windowNumber:window.windowNumber context:nil
                        characters:@"\x1b" charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:53];
                    [NSApp postEvent:event atStart:YES];
                }];
                if (screenshot) screenshotTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:NO block:^(NSTimer * __unused timer) {
                    imageError = captureWindow(window,screenshot);
                }];
                [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
                [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                const double began = CACurrentMediaTime();
                [NSApp run];
                elapsed = CACurrentMediaTime()-began;
                [NSNotificationCenter.defaultCenter removeObserver:observer];
                window.inputHandler = nil;
                [NSApp setPresentationOptions:previousPresentation];
                [view stopAnimation];
                [window close];
                exitReason = reason ? reason : @"event_loop_returned";
                screenshotError = imageError;
                if (screenshot && !screenshotError && ![[NSFileManager defaultManager] fileExistsAtPath:screenshot])
                    screenshotError = @"Window capture was not reached before exit.";
                if (testInput && ![exitReason isEqualToString:@"key_input"])
                    throw std::runtime_error("The posted input did not exit through the key event path");
                frameCallbacks = callbacks;
                if (view.isAnimating || view.hasActiveSource) throw std::runtime_error("Saver did not stop after full-screen exit");
                if (elapsed > 7) throw std::runtime_error("Full-screen module host exceeded its exit bound");
            }
            const NSUInteger callbacksAfterStop = view.sourceCallbackCount;
            [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
            if (view.hasActiveSource || view.sourceCallbackCount != callbacksAfterStop)
                throw std::runtime_error("Source activity continued after stop");
            NSDictionary *persistentAfter = [defaults persistentDomainForName:ModuleName];
            if (persistentBefore != persistentAfter && ![persistentBefore isEqual:persistentAfter])
                throw std::runtime_error("Persistent screensaver settings changed during the test");
            NSDictionary *result = @{ @"host": @"ScreenSaverView module host; not ScreenSaverEngine",
                @"fullscreen": @(fullscreen), @"exit_reason": exitReason, @"elapsed_seconds": @(elapsed),
                @"frame_callbacks": @(frameCallbacks), @"synthetic_input_requested": @(testInput),
                @"source_active_after_stop": @(view.hasActiveSource),
                @"source_callbacks_before_drain": @(callbacksAfterStop), @"source_callbacks_after_drain": @(view.sourceCallbackCount),
                @"persistent_settings_unchanged": @YES, @"screenshot_error": screenshotError ? screenshotError : @"" };
            NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:&error];
            if (!json) throw std::runtime_error(error.localizedDescription.UTF8String);
            std::fwrite(json.bytes,1,json.length,stdout); std::fputc('\n',stdout);
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"FAH saver smoke: %s\n",exception.what());
            return 1;
        }
    }
}
