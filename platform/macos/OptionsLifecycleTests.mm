#import <AppKit/AppKit.h>
#import <ScreenSaver/ScreenSaver.h>
#import "FAHSaverView.h"

#include <cstdio>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static NSView *namedControl(NSView *root, NSString *name) {
    if ([root.accessibilityLabel isEqualToString:name]) return root;
    for (NSView *child in root.subviews)
        if (NSView *found = namedControl(child,name)) return found;
    return nil;
}

struct TestPreferences {
    __strong NSString *name = [@"org.fahsaver.options-test." stringByAppendingString:NSUUID.UUID.UUIDString];
    __strong NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:name];
    ~TestPreferences() { [defaults removePersistentDomainForName:name]; }
};

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            check(argc == 3,"Supply the saver bundle and capture fixture");
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
            NSError *error = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:argv[1]]];
            check(bundle && [bundle loadAndReturnError:&error],"Could not load the saver bundle");
            Class principal = bundle.principalClass;
            check(principal && [principal isSubclassOfClass:ScreenSaverView.class],"Invalid saver class");
            TestPreferences preferences;
            [preferences.defaults setInteger:1 forKey:@"inputMode"];
            [preferences.defaults setObject:[[NSString stringWithUTF8String:argv[2]] stringByStandardizingPath]
                                    forKey:@"capturePath"];
            FAHSaverView *view = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240)
                isPreview:YES preferences:preferences.defaults];
            NSWindow *parent = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,640,480)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            parent.releasedWhenClosed = NO;
            parent.alphaValue = 0;
            parent.ignoresMouseEvents = YES;
            unsigned saves = 0, cancels = 0;
            for (unsigned cycle=0; cycle<20; ++cycle) {
                @autoreleasepool {
                    NSWindow *sheet = view.configureSheet;
                    check(sheet && sheet.contentView,"Options sheet was not created");
                    check(!sheet.releasedWhenClosed,"Options window enables unsafe automatic release under ARC");
                    sheet.alphaValue = 0;
                    sheet.ignoresMouseEvents = YES;
                    const auto before = [preferences.defaults persistentDomainForName:preferences.name];
                    NSPopUpButton *background = (NSPopUpButton *)namedControl(sheet.contentView,@"Background");
                    NSPopUpButton *playback = (NSPopUpButton *)namedControl(sheet.contentView,@"Snapshot playback");
                    NSTextField *interval = (NSTextField *)namedControl(sheet.contentView,@"Seconds between snapshots");
                    check(background && playback && interval,"Options controls were lost on reopen");
                    [playback selectItemAtIndex:1];
                    [NSApp sendAction:playback.action to:playback.target from:playback];
                    const double seconds = .5 + .25*cycle;
                    interval.doubleValue = seconds;
                    const NSInteger selection = cycle%3;
                    [background selectItemAtIndex:selection];
                    __block bool ended = false;
                    const bool save = cycle%2 == 0;
                    [parent beginSheet:sheet completionHandler:^(NSModalResponse response) {
                        check(response == (save ? NSModalResponseOK : NSModalResponseCancel),"Wrong sheet completion result");
                        ended = true;
                    }];
                    check(sheet.sheetParent == parent,"Options sheet did not attach to its host");
                    NSButton *button = nil;
                    for (NSView *child in sheet.contentView.subviews)
                        if ([child isKindOfClass:NSButton.class] &&
                            [[(NSButton *)child title] isEqualToString:save ? @"Save" : @"Cancel"])
                            button = (NSButton *)child;
                    check(button != nil,"Options dismissal button is missing");
                    check([NSApp sendAction:button.action to:button.target from:button],"Options action was not handled");
                    const auto deadline = [NSDate dateWithTimeIntervalSinceNow:1];
                    while (!ended && deadline.timeIntervalSinceNow > 0)
                        [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
                    check(ended && !sheet.sheetParent && !parent.attachedSheet,"Options sheet did not finish dismissal");
                    check(sheet.alphaValue == 0 && parent.alphaValue == 0 && !view.hasActiveSource,
                          "Hidden options test exposed a window or source");
                    if (save) {
                        check([preferences.defaults doubleForKey:@"snapshotInterval"] == seconds &&
                              [preferences.defaults boolForKey:@"loop"] &&
                              [preferences.defaults boolForKey:@"sky"] == (selection != 2) &&
                              [preferences.defaults boolForKey:@"darkSky"] == (selection == 1),
                              "Saved options were not preserved");
                        ++saves;
                    } else {
                        check([before isEqual:[preferences.defaults persistentDomainForName:preferences.name]],
                              "Cancel changed saved preferences");
                        ++cancels;
                    }
                    [sheet close];
                }
            }
            [parent close];
            std::printf("Options lifecycle passed: %u saves, %u cancels, 20 sheet closures/reopens; isolated preferences\n",saves,cancels);
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"Options lifecycle: %s\n",exception.what());
            return 1;
        }
    }
}
