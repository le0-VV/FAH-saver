#import "FAHSaverView.h"
#include "fah/Source.hpp"

#include <memory>
#include <string>

static NSString * const FAHDefaultsName = @"org.fahsaver.screensaver";

@implementation FAHSaverView {
    MTKView *_metalView;
    FAHRenderer *_renderer;
    NSTextField *_status;
    ScreenSaverDefaults *_defaults;
    FAHRenderOptions _options;
    std::unique_ptr<fah::Source> _source;
    std::shared_ptr<fah::Capture> _liveCapture;
    NSString *_connectionStatus;
    NSWindow *_sheet;
    NSPopUpButton *_inputMode, *_playback, *_style, *_background;
    NSTextField *_sourceURL, *_unit, *_capturePath, *_fps;
    NSColorWell *_colour;
}

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview {
    self = [super initWithFrame:frame isPreview:isPreview];
    if (!self) return nil;
    _defaults = [ScreenSaverDefaults defaultsForModuleWithName:FAHDefaultsName];
    [_defaults registerDefaults:@{ @"inputMode": @0,
        @"sourceURL": @"ws://127.0.0.1:7396/api/websocket", @"unit": @"",
        @"capturePath": @"", @"loop": @NO, @"style": @0, @"fps": @60,
        @"sky": @YES, @"red": @.02, @"green": @.025, @"blue": @.035 }];
    NSError *error = nil;
    _renderer = [[FAHRenderer alloc] initWithDevice:MTLCreateSystemDefaultDevice() error:&error];
    _metalView = [[MTKView alloc] initWithFrame:self.bounds device:_renderer.device];
    _metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _metalView.framebufferOnly = YES;
    _metalView.paused = YES;
    _metalView.enableSetNeedsDisplay = NO;
    _metalView.delegate = _renderer;
    [self addSubview:_metalView];
    _status = [NSTextField wrappingLabelWithString:error.localizedDescription ? error.localizedDescription : @"Connecting to Folding@home…"];
    _status.textColor = NSColor.whiteColor;
    _status.alignment = NSTextAlignmentCenter;
    _status.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_status];
    [NSLayoutConstraint activateConstraints:@[
        [_status.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [_status.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_status.widthAnchor constraintLessThanOrEqualToAnchor:self.widthAnchor multiplier:.85] ]];
    [self loadOptions];
    return self;
}

- (BOOL)loadOptions {
    _options.fps = static_cast<int>([_defaults integerForKey:@"fps"]);
    _options.loop = [_defaults boolForKey:@"loop"];
    const NSInteger style = [_defaults integerForKey:@"style"];
    _options.style = style < 3 ? static_cast<fah::Style>(style+1) : fah::Style::SpaceFilling;
    _options.styleMode = style == 3 ? FAHStyleMode::Cycle : style == 4 ? FAHStyleMode::Random : FAHStyleMode::Fixed;
    _options.sky = [_defaults boolForKey:@"sky"];
    _options.colour = {[_defaults floatForKey:@"red"], [_defaults floatForKey:@"green"], [_defaults floatForKey:@"blue"]};
    try {
        if (style < 0 || style > 4 || [_defaults integerForKey:@"inputMode"] < 0 || [_defaults integerForKey:@"inputMode"] > 1)
            throw std::runtime_error("Invalid saved screensaver settings. Open Screen Saver Options to correct them.");
        [_renderer setOptions:_options];
        self.animationTimeInterval = 1.0/_options.fps;
        return YES;
    } catch (const std::exception& exception) {
        _connectionStatus = [NSString stringWithUTF8String:exception.what()];
        return NO;
    }
}

- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture {
    [_renderer setCapture:std::move(capture)];
    [self updateStatus];
}

- (void)updateStatus {
    NSString *text = _connectionStatus ? _connectionStatus : _renderer.lastError;
    NSString *display = text ? text : @"";
    if (![_status.stringValue isEqualToString:display]) _status.stringValue = display;
    _status.hidden = !text.length;
}

- (void)startAnimation {
    [super startAnimation];
    if (!_renderer) return;
    if (_source) { _source->stop(); _source.reset(); }
    _connectionStatus = nil;
    if (![self loadOptions]) { [self updateStatus]; return; }
    [_renderer setCapture:nullptr];
    [_renderer start];
    try {
        if ([_defaults integerForKey:@"inputMode"] == 1) {
            NSString *path = [_defaults stringForKey:@"capturePath"];
            if (!path.length) throw std::runtime_error("Choose a protein capture file in Screen Saver Options.");
            [self setCapture:std::make_shared<const fah::Capture>(fah::readCapture(path.fileSystemRepresentation))];
        } else {
            __weak FAHSaverView *weakSelf = self;
            fah::SourceOptions sourceOptions{[[_defaults stringForKey:@"sourceURL"] UTF8String],
                [[_defaults stringForKey:@"unit"] UTF8String]};
            _source = fah::makeSource(sourceOptions, [weakSelf](const fah::SourceEvent& event) {
                FAHSaverView *view = weakSelf;
                if (!view) return;
                switch (event.kind) {
                    case fah::SourceEvent::Kind::Reset:
                        view->_liveCapture = std::make_shared<fah::Capture>();
                        [view setCapture:view->_liveCapture];
                        break;
                    case fah::SourceEvent::Kind::Topology:
                        view->_liveCapture = std::make_shared<fah::Capture>();
                        view->_liveCapture->topology = *event.topology;
                        [view setCapture:view->_liveCapture];
                        break;
                    case fah::SourceEvent::Kind::Frame:
                        if (!view->_liveCapture || !event.frame) break;
                        if (!view->_options.loop) view->_liveCapture->frames.clear();
                        view->_liveCapture->frames.push_back(*event.frame);
                        view->_connectionStatus = nil;
                        [view setCapture:view->_liveCapture];
                        break;
                    case fah::SourceEvent::Kind::Status:
                    case fah::SourceEvent::Kind::Error:
                        view->_connectionStatus = [NSString stringWithUTF8String:event.text.c_str()];
                        [view updateStatus];
                        break;
                }
            });
            _connectionStatus = @"Connecting to Folding@home…";
            _source->start();
        }
    } catch (const std::exception& exception) {
        _connectionStatus = [NSString stringWithUTF8String:exception.what()];
        [_renderer setCapture:nullptr];
    }
    [self updateStatus];
}

- (void)animateOneFrame {
    if (!self.isAnimating) return;
    [_metalView draw];
    [self updateStatus];
}

- (void)stopAnimation {
    if (_source) { _source->stop(); _source.reset(); }
    [_renderer stop];
    [_renderer setCapture:nullptr];
    _liveCapture.reset();
    [super stopAnimation];
}

- (void)dealloc { if (_source) _source->stop(); [_renderer stop]; }
- (BOOL)hasConfigureSheet { return YES; }

- (NSWindow *)configureSheet {
    if (_sheet) return _sheet;
    _sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,580,400)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    _sheet.title = @"FAH Saver Options";
    NSView *content = _sheet.contentView;
    auto label = [&](NSString *title, CGFloat y) {
        NSTextField *field = [NSTextField labelWithString:title];
        field.frame = NSMakeRect(20,y,140,24); [content addSubview:field];
    };
    auto text = [&](NSString *value, CGFloat y) -> NSTextField * {
        NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(170,y,385,24)];
        field.stringValue = value ? value : @""; [content addSubview:field]; return field;
    };
    auto popup = [&](NSArray<NSString *> *items, CGFloat y) -> NSPopUpButton * {
        NSPopUpButton *button = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(170,y,240,26) pullsDown:NO];
        [button addItemsWithTitles:items]; [content addSubview:button]; return button;
    };
    label(@"Protein source",350);
    _inputMode = popup(@[@"Live Folding@home client", @"Capture file"],350);
    [_inputMode selectItemAtIndex:[_defaults integerForKey:@"inputMode"]];
    label(@"Source URL",315); _sourceURL = text([_defaults stringForKey:@"sourceURL"],315);
    _sourceURL.accessibilityLabel = @"Folding@home source URL";
    label(@"WU ID (optional)",280); _unit = text([_defaults stringForKey:@"unit"],280);
    _unit.accessibilityLabel = @"Optional work unit ID";
    label(@"Capture file",245); _capturePath = text([_defaults stringForKey:@"capturePath"],245);
    _capturePath.accessibilityLabel = @"Protein capture file path";
    _capturePath.frame = NSMakeRect(170,245,300,24);
    NSButton *browse = [NSButton buttonWithTitle:@"Browse…" target:self action:@selector(browseCapture:)];
    browse.frame = NSMakeRect(475,244,80,26); [content addSubview:browse];
    label(@"Snapshots",210); _playback = popup(@[@"Follow latest", @"Loop available frames"],210);
    [_playback selectItemAtIndex:[_defaults boolForKey:@"loop"] ? 1 : 0];
    label(@"Model style",175); _style = popup(@[@"Space-filling",@"Ball-and-stick",@"Sticks",@"Cycle styles",@"Random styles"],175);
    [_style selectItemAtIndex:[_defaults integerForKey:@"style"]];
    label(@"Frame-rate limit",140); _fps = text([[_defaults objectForKey:@"fps"] stringValue],140);
    _fps.frame = NSMakeRect(170,140,90,24);
    _fps.accessibilityLabel = @"Frame-rate limit in frames per second";
    NSTextField *unitLabel = [NSTextField labelWithString:@"fps (1–240)"];
    unitLabel.frame = NSMakeRect(270,140,140,24); [content addSubview:unitLabel];
    label(@"Background",105); _background = popup(@[@"Sky and grid",@"Solid colour"],105);
    [_background selectItemAtIndex:[_defaults boolForKey:@"sky"] ? 0 : 1];
    _colour = [[NSColorWell alloc] initWithFrame:NSMakeRect(430,105,65,26)];
    _colour.color = [NSColor colorWithSRGBRed:[_defaults floatForKey:@"red"]
        green:[_defaults floatForKey:@"green"] blue:[_defaults floatForKey:@"blue"] alpha:1];
    _inputMode.accessibilityLabel = @"Protein source type";
    _playback.accessibilityLabel = @"Snapshot playback";
    _style.accessibilityLabel = @"Model style";
    _background.accessibilityLabel = @"Background";
    _colour.accessibilityLabel = @"Solid background colour";
    [content addSubview:_colour];
    NSButton *save = [NSButton buttonWithTitle:@"Save" target:self action:@selector(saveOptions:)];
    save.frame = NSMakeRect(460,20,95,32); save.keyEquivalent = @"\r"; [content addSubview:save];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancelOptions:)];
    cancel.frame = NSMakeRect(355,20,95,32); cancel.keyEquivalent = @"\x1b"; [content addSubview:cancel];
    return _sheet;
}

- (void)browseCapture:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO; panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK) _capturePath.stringValue = panel.URL.path;
}

- (void)saveOptions:(id)sender {
    NSScanner *scanner = [NSScanner scannerWithString:_fps.stringValue];
    NSInteger fps;
    BOOL valid = [scanner scanInteger:&fps] && scanner.isAtEnd && fps >= 1 && fps <= 240;
    NSString *problem = valid ? nil : @"Enter a whole frame-rate limit from 1 to 240 fps.";
    if (!problem && _inputMode.indexOfSelectedItem == 0) {
        NSURL *url = [NSURL URLWithString:_sourceURL.stringValue];
        if (!url.host.length || !([url.scheme isEqualToString:@"ws"] || [url.scheme isEqualToString:@"wss"]))
            problem = @"Enter a WebSocket source URL beginning with ws:// or wss://.";
    }
    if (!problem && _inputMode.indexOfSelectedItem == 1) {
        BOOL isDirectory = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:_capturePath.stringValue isDirectory:&isDirectory] || isDirectory)
            problem = @"Choose an existing protein capture file.";
    }
    if (problem) { NSAlert *alert = [NSAlert new]; alert.messageText = problem; [alert runModal]; return; }
    [_defaults setInteger:_inputMode.indexOfSelectedItem forKey:@"inputMode"];
    [_defaults setObject:_sourceURL.stringValue forKey:@"sourceURL"];
    [_defaults setObject:_unit.stringValue forKey:@"unit"];
    [_defaults setObject:_capturePath.stringValue forKey:@"capturePath"];
    [_defaults setBool:_playback.indexOfSelectedItem == 1 forKey:@"loop"];
    [_defaults setInteger:_style.indexOfSelectedItem forKey:@"style"];
    [_defaults setInteger:fps forKey:@"fps"];
    [_defaults setBool:_background.indexOfSelectedItem == 0 forKey:@"sky"];
    NSColor *colour = [_colour.color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    [_defaults setFloat:colour.redComponent forKey:@"red"];
    [_defaults setFloat:colour.greenComponent forKey:@"green"];
    [_defaults setFloat:colour.blueComponent forKey:@"blue"];
    [_defaults synchronize];
    [NSApp endSheet:_sheet returnCode:NSModalResponseOK];
    [_sheet orderOut:nil]; _sheet = nil;
}

- (void)cancelOptions:(id)sender {
    [NSApp endSheet:_sheet returnCode:NSModalResponseCancel];
    [_sheet orderOut:nil]; _sheet = nil;
}
@end
