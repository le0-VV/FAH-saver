#import <AppKit/AppKit.h>
#import <ScreenSaver/ScreenSaver.h>
#import "FAHSaverView.h"
#include "TestSource.hpp"
#include "fah/DecimalInput.hpp"

#include <cstdio>
#include <stdexcept>
#include <limits>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static NSView *namedControl(NSView *root, NSString *name) {
    if ([root.accessibilityLabel isEqualToString:name]) return root;
    for (NSView *child in root.subviews)
        if (NSView *found = namedControl(child,name)) return found;
    return nil;
}

static NSButton *namedButton(NSWindow *sheet, NSString *title) {
    for (NSView *child in sheet.contentView.subviews)
        if ([child isKindOfClass:NSButton.class] && [[(NSButton *)child title] isEqualToString:title])
            return (NSButton *)child;
    throw std::runtime_error("Options dismissal button is missing");
}

static void sendButton(NSWindow *sheet, NSString *title) {
    NSButton *button = namedButton(sheet,title);
    check([NSApp sendAction:button.action to:button.target from:button],"Options action was not handled");
}

static void waitForSheet(bool &ended) {
    const auto deadline = [NSDate dateWithTimeIntervalSinceNow:1];
    while (!ended && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    check(ended,"Options sheet did not finish dismissal");
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
            // Old file-only preferences must no longer affect the user flow.
            [preferences.defaults setInteger:1 forKey:@"inputMode"];
            [preferences.defaults setObject:@"unused-retired-capture.json" forKey:@"capturePath"];
            TemporaryModelCache cache;
            auto factory = fixtureSource(std::make_shared<const fah::Capture>(fah::readCapture(argv[2])));
            FAHSaverView *view = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240)
                isPreview:YES preferences:preferences.defaults cachePath:cache.file() sourceFactory:factory];
            NSTextField *initialInterval = (NSTextField *)namedControl(view.configureSheet.contentView,@"Seconds between snapshots");
            check(initialInterval.doubleValue == 1,"New settings did not default to one second");
            NSFormatter *formatter = initialInterval.formatter;
            check(formatter != nil,"Interval edit formatter is missing");
            for (NSString *text in @[@"",@"."]) {
                id object = nil;
                check([formatter getObjectValue:&object forString:text errorDescription:nil] && [object isEqual:text],
                      "Partial text would trigger a formatter commit alert");
            }
            NSWindow *parent = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,640,480)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            parent.releasedWhenClosed = NO;
            parent.alphaValue = 0;
            parent.ignoresMouseEvents = YES;
            {
                NSWindow *sheet = view.configureSheet;
                sheet.alphaValue = 0; sheet.ignoresMouseEvents = YES;
                NSPopUpButton *playback = (NSPopUpButton *)namedControl(sheet.contentView,@"Snapshot playback");
                [playback selectItemAtIndex:1]; [NSApp sendAction:playback.action to:playback.target from:playback];
                NSTextField *field = (NSTextField *)namedControl(sheet.contentView,@"Seconds between snapshots");
                __block bool ended = false;
                [parent beginSheet:sheet completionHandler:^(NSModalResponse response) { ended = response == NSModalResponseCancel; }];
                [sheet makeFirstResponder:field];
                NSTextView *editor = (NSTextView *)field.currentEditor;
                check([editor isKindOfClass:NSTextView.class],"Native interval field editor did not start");
                for (NSString *replacement in @[@"a",@"1e3",@"1..2",@"-2",@"+1",@"1,5",@" 1",@"１",@"١"]) {
                    editor.string = @"1.25"; editor.selectedRange = NSMakeRange(0,4);
                    [editor insertText:replacement replacementRange:editor.selectedRange];
                    check([editor.string isEqualToString:@"1.25"] && NSEqualRanges(editor.selectedRange,NSMakeRange(0,4)),
                          "Native editor accepted non-decimal replacement text or changed its selection");
                }
                editor.string = @"1.25"; editor.selectedRange = NSMakeRange(2,0);
                [editor insertText:@"." replacementRange:editor.selectedRange];
                check([editor.string isEqualToString:@"1.25"] && NSEqualRanges(editor.selectedRange,NSMakeRange(2,0)),
                      "Native editor accepted a second decimal point or changed its selection");
                editor.string = @"0"; editor.selectedRange = NSMakeRange(0,1);
                NSEvent *letter = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
                    timestamp:0 windowNumber:sheet.windowNumber context:nil characters:@"a" charactersIgnoringModifiers:@"a"
                    isARepeat:NO keyCode:0];
                [editor keyDown:letter];
                check([editor.string isEqualToString:@"0"],"Native keyDown accepted a letter");
                NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
                [pasteboard setString:@"12abc.3" forType:NSPasteboardTypeString];
                editor.string = @"1.25"; editor.selectedRange = NSMakeRange(0,4);
                [editor readSelectionFromPasteboard:pasteboard type:NSPasteboardTypeString];
                check([editor.string isEqualToString:@"1.25"],"Native paste path changed the field for invalid decimal text");
                [pasteboard releaseGlobally];
                for (NSString *replacement in @[@"",@".",@"0.",@".5",@"120.5"]) {
                    editor.string = @"1.25"; editor.selectedRange = NSMakeRange(0,4);
                    [editor insertText:replacement replacementRange:editor.selectedRange];
                    check([editor.string isEqualToString:replacement],"Native editor rejected valid decimal replacement");
                }
                sendButton(sheet,@"Cancel"); waitForSheet(ended); [sheet close];
            }
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
                    check(!namedControl(sheet.contentView,@"Protein capture file path") &&
                          !namedControl(sheet.contentView,@"Protein source type"),"Capture-file controls remain in Options");
                    [playback selectItemAtIndex:1];
                    [NSApp sendAction:playback.action to:playback.target from:playback];
                    const double seconds = 1 + .25*cycle;
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
                    sendButton(sheet,save ? @"Save" : @"Cancel");
                    waitForSheet(ended);
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
            NSWindow *sheet = view.configureSheet;
            sheet.alphaValue = 0; sheet.ignoresMouseEvents = YES;
            NSTextField *inlineError = (NSTextField *)namedControl(sheet.contentView,@"Options validation error");
            NSTextField *interval = (NSTextField *)namedControl(sheet.contentView,@"Seconds between snapshots");
            NSPopUpButton *playback = (NSPopUpButton *)namedControl(sheet.contentView,@"Snapshot playback");
            check(inlineError && interval && playback,"Inline interval controls are missing");
            [playback selectItemAtIndex:1];
            [NSApp sendAction:playback.action to:playback.target from:playback];
            __block bool correctedSaveEnded = false;
            [parent beginSheet:sheet completionHandler:^(NSModalResponse response) { correctedSaveEnded = response == NSModalResponseOK; }];
            const auto beforeInvalid = [preferences.defaults persistentDomainForName:preferences.name];
            auto reject = [&](NSTextField *field, NSString *text) {
                [sheet makeFirstResponder:nil]; field.stringValue = text;
                const auto modal = NSApp.modalWindow;
                sendButton(sheet,@"Save");
                check(sheet.sheetParent == parent && !sheet.attachedSheet && NSApp.modalWindow == modal,
                      "Invalid input opened a modal alert or closed the options sheet");
                check(!inlineError.hidden && inlineError.stringValue.length &&
                      [beforeInvalid isEqual:[preferences.defaults persistentDomainForName:preferences.name]],
                      "Invalid input changed saved settings or lacked inline feedback");
                check(NSMaxY(inlineError.frame) <= NSMinY(field.frame) && NSMinX(inlineError.frame) >= NSMinX(field.frame),
                      "Inline error is not directly below the offending field");
                check([inlineError.stringValue sizeWithAttributes:@{NSFontAttributeName:inlineError.font}].width <= inlineError.frame.size.width,
                      "Inline message is too wide for its field-local space");
                for (NSView *control in sheet.contentView.subviews)
                    if (control != inlineError && !control.hidden)
                        check(!NSIntersectsRect(inlineError.frame,control.frame),"Inline error overlaps another form row");
                check([field.stringValue isEqualToString:text] && field.currentEditor == sheet.firstResponder,
                      "Invalid input or field focus was lost");
            };
            for (NSString *text in @[@"",@".",@"0",@"0.01",@"1e3",[@"9" stringByPaddingToLength:400 withString:@"9" startingAtIndex:0]])
                reject(interval,text);
            [playback selectItemAtIndex:0];
            [NSApp sendAction:playback.action to:playback.target from:playback];
            check(inlineError.hidden,"Changing playback did not clear inline feedback");
            [playback selectItemAtIndex:1];
            [NSApp sendAction:playback.action to:playback.target from:playback];
            [sheet makeFirstResponder:nil]; interval.stringValue = @"600";
            NSTextField *fps = (NSTextField *)namedControl(sheet.contentView,@"Frame-rate limit in frames per second");
            reject(fps,@"1.5");
            [sheet makeFirstResponder:nil]; fps.stringValue = @"60";
            NSTextField *source = (NSTextField *)namedControl(sheet.contentView,@"Folding@home source URL");
            NSString *originalSource = source.stringValue;
            reject(source,@"file:///not-a-live-source");
            [sheet makeFirstResponder:nil]; source.stringValue = originalSource;
            [sheet makeFirstResponder:interval];
            interval.currentEditor.string = @"600";
            [NSNotificationCenter.defaultCenter postNotificationName:NSControlTextDidChangeNotification object:interval];
            check(inlineError.hidden,"Editing did not clear the inline message");
            sendButton(sheet,@"Save"); waitForSheet(correctedSaveEnded);
            check([preferences.defaults doubleForKey:@"snapshotInterval"] == 600,"Corrected interval above 60 seconds did not save");
            [sheet close];
            for (double seconds : {.5, 1e100, std::numeric_limits<double>::max()}) {
                [preferences.defaults setDouble:seconds forKey:@"snapshotInterval"];
                NSWindow *reopened = view.configureSheet;
                reopened.alphaValue = 0; reopened.ignoresMouseEvents = YES;
                NSTextField *field = (NSTextField *)namedControl(reopened.contentView,@"Seconds between snapshots");
                check(fah::decimalPartial(field.stringValue.UTF8String) && field.doubleValue == seconds,
                      "Saved interval was changed or reopened with illegal exponent text");
                __block bool ended = false;
                [parent beginSheet:reopened completionHandler:^(NSModalResponse response) { ended = response == NSModalResponseOK; }];
                sendButton(reopened,@"Save"); waitForSheet(ended);
                check([preferences.defaults doubleForKey:@"snapshotInterval"] == seconds,"Large or explicit saved interval did not round-trip");
                [reopened close];
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
