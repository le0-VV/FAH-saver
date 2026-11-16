#import "FAHSaverView.h"
#import <os/log.h>
#include "fah/LiveModel.hpp"
#include "fah/SnapshotCache.hpp"

#include <memory>
#include <cmath>
#include <string>

static NSString * const FAHDefaultsName = @"org.fahsaver.screensaver";

static std::filesystem::path defaultCachePath() {
    NSURL *directory = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    if (!directory) throw std::runtime_error("Could not locate the model cache directory");
    return std::filesystem::path(directory.path.fileSystemRepresentation) / "org.fahsaver.screensaver" / "latest-model.cache";
}

static void cacheError(const std::string& message) {
    static os_log_t log = os_log_create("org.fahsaver.screensaver", "cache");
    os_log_error(log, "%{public}s", message.c_str());
}

@implementation FAHSaverView {
    MTKView *_metalView;
    FAHRenderer *_renderer;
    NSTextField *_status;
    NSUserDefaults *_defaults;
    FAHRenderOptions _options;
    std::unique_ptr<fah::Source> _source;
    FAHSaverSourceFactory _sourceFactory;
    std::filesystem::path _cachePath;
    std::string _sourceSelector;
    std::unique_ptr<fah::SnapshotCache> _cache;
    fah::LiveModel _model;
    NSString *_connectionStatus, *_configurationError;
    NSUInteger _sourceGeneration, _sourceCallbackCount, _animationCallbackCount, _measurementCallbackBaseline;
    NSWindow *_sheet;
    NSPopUpButton *_playback, *_style, *_background;
    NSTextField *_sourceURL, *_unit, *_fps, *_snapshotInterval, *_snapshotIntervalUnit;
    NSColorWell *_colour;
}

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview {
    return [self initWithFrame:frame isPreview:isPreview preferences:nil];
}

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview preferences:(NSUserDefaults *)preferences {
    return [self initWithFrame:frame isPreview:isPreview preferences:preferences
        cachePath:defaultCachePath() sourceFactory:fah::makeSource];
}

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview preferences:(NSUserDefaults *)preferences
                    cachePath:(const std::filesystem::path&)cachePath sourceFactory:(FAHSaverSourceFactory)sourceFactory {
    self = [super initWithFrame:frame isPreview:isPreview];
    if (!self) return nil;
    if (!sourceFactory) throw std::runtime_error("A FAH source factory is required");
    _sourceFactory = std::move(sourceFactory);
    _cachePath = cachePath;
    // Keep the Metal child inside a layer-backed root when macOS composites each display.
    self.wantsLayer = YES;
    _defaults = preferences ? preferences : [ScreenSaverDefaults defaultsForModuleWithName:FAHDefaultsName];
    [_defaults registerDefaults:@{ @"sourceURL": @"ws://127.0.0.1:7396/api/websocket", @"unit": @"",
        @"loop": @NO, @"style": @0, @"fps": @60,
        @"sky": @YES, @"darkSky": @NO, @"snapshotInterval": @.5,
        @"red": @.02, @"green": @.025, @"blue": @.035 }];
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
    _status = [NSTextField wrappingLabelWithString:error.localizedDescription ?: @"Connecting to Folding@home…"];
    _status.textColor = NSColor.whiteColor;
    _status.drawsBackground = YES;
    _status.backgroundColor = [NSColor colorWithWhite:0 alpha:.75];
    _status.alignment = NSTextAlignmentCenter;
    _status.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_status];
    [NSLayoutConstraint activateConstraints:@[
        [_status.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [_status.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_status.widthAnchor constraintLessThanOrEqualToAnchor:self.widthAnchor multiplier:.85] ]];
    _connectionStatus = error.localizedDescription ?: @"Connecting to Folding@home…";
    if (!_renderer) _configurationError = error.localizedDescription ?: @"The Metal renderer could not be created.";
    [self loadOptions];
    [self prepareCache];
    [self updateStatus];
    return self;
}

- (NSString *)sourceURL {
    NSString *url = [_defaults stringForKey:@"sourceURL"];
    // Retired file-only settings can have an empty source; use the normal local endpoint.
    return url.length ? url : @"ws://127.0.0.1:7396/api/websocket";
}

- (fah::SourceOptions)sourceOptions {
    return {self.sourceURL.UTF8String, [[_defaults stringForKey:@"unit"] UTF8String] ?: ""};
}

- (void)prepareCache {
    const auto options = [self sourceOptions];
    std::string selector = options.url;
    selector.push_back('\0'); selector += options.unit;
    if (selector != _sourceSelector) {
        _cache.reset();
        _model.clear();
        _sourceSelector = selector;
    }
    try {
        if (!_cache) _cache = std::make_unique<fah::SnapshotCache>(_cachePath, selector, cacheError);
        if (!_model.available())
            if (auto snapshot = _cache->load()) _model.seed(std::move(snapshot));
    } catch (const std::exception& exception) {
        cacheError(exception.what());
    }
    [_renderer setCapture:_model.capture()];
}

- (BOOL)loadOptions {
    if (!_renderer) return NO;
    _options.fps = static_cast<int>([_defaults integerForKey:@"fps"]);
    _options.loop = [_defaults boolForKey:@"loop"];
    _options.snapshotInterval = [_defaults doubleForKey:@"snapshotInterval"];
    const NSInteger style = [_defaults integerForKey:@"style"];
    _options.style = style < 3 ? static_cast<fah::Style>(style+1) : fah::Style::SpaceFilling;
    _options.styleMode = style == 3 ? FAHStyleMode::Cycle : style == 4 ? FAHStyleMode::Random : FAHStyleMode::Fixed;
    _options.sky = [_defaults boolForKey:@"sky"];
    _options.darkSky = [_defaults boolForKey:@"darkSky"];
    _options.colour = {[_defaults floatForKey:@"red"], [_defaults floatForKey:@"green"], [_defaults floatForKey:@"blue"]};
    try {
        if (style < 0 || style > 4)
            throw std::runtime_error("Invalid saved screensaver settings. Open Screen Saver Options to correct them.");
        [_renderer setOptions:_options];
        self.animationTimeInterval = 1.0/_options.fps;
        _configurationError = nil;
        return YES;
    } catch (const std::exception& exception) {
        _configurationError = [NSString stringWithUTF8String:exception.what()];
        return NO;
    }
}

- (void)beginMeasurement {
    _measurementCallbackBaseline = _animationCallbackCount;
    [_renderer beginMeasurement];
}
- (NSDictionary *)renderMetrics {
    NSMutableDictionary *result = [[_renderer metrics] mutableCopy];
    result[@"drawable_width"] = @(_metalView.drawableSize.width);
    result[@"drawable_height"] = @(_metalView.drawableSize.height);
    result[@"drawing_mode"] = @"ScreenSaverView animateOneFrame";
    result[@"animation_callbacks"] = @(_animationCallbackCount-_measurementCallbackBaseline);
    result[@"model_available"] = @(_model.available());
    result[@"using_cached_model"] = @(_model.usingFallback());
    result[@"connection_message_visible"] = @(!_status.hidden);
    return result;
}

- (BOOL)hasActiveSource { return static_cast<bool>(_source); }
- (NSUInteger)sourceCallbackCount { return _sourceCallbackCount; }
- (NSUInteger)animationCallbackCount { return _animationCallbackCount; }

- (void)updateStatus {
    NSString *text = _configurationError ?: (_renderer && _model.available() ? _renderer.lastError : (_connectionStatus ?: _renderer.lastError));
    NSString *display = text ?: @"";
    if (![_status.stringValue isEqualToString:display]) _status.stringValue = display;
    _status.hidden = !text.length;
}

- (void)receiveSourceEvent:(const fah::SourceEvent&)event {
    const auto previous = _model.revision();
    const bool fresh = _model.receive(event, _options.loop);
    if (_model.revision() != previous) [_renderer setCapture:_model.capture()];
    if (event.kind == fah::SourceEvent::Kind::Status || event.kind == fah::SourceEvent::Kind::Error)
        _connectionStatus = [NSString stringWithUTF8String:event.text.c_str()];
    else if (fresh) _connectionStatus = nil;
    if (fresh && _cache) {
        try { _cache->queue(_model.latestTopology(), _model.latestFrame()); }
        catch (const std::exception& exception) { cacheError(exception.what()); }
    }
    [self updateStatus];
}

- (void)startAnimation {
    [super startAnimation];
    if (!_renderer) return;
    ++_sourceGeneration;
    if (_source) { _source->stop(); _source.reset(); }
    fah::SourceEvent reset;
    reset.kind = fah::SourceEvent::Kind::Reset;
    _model.receive(reset, _options.loop);
    _connectionStatus = @"Connecting to Folding@home…";
    if (![self loadOptions]) { [self updateStatus]; return; }
    [self prepareCache];
    [_renderer start];
    try {
        __weak FAHSaverView *weakSelf = self;
        const NSUInteger generation = _sourceGeneration;
        _source = _sourceFactory([self sourceOptions], [weakSelf,generation](fah::SourceEvent event) {
            FAHSaverView *view = weakSelf;
            if (!view || generation != view->_sourceGeneration || !view.isAnimating) return;
            ++view->_sourceCallbackCount;
            [view receiveSourceEvent:event];
        });
        if (!_source) throw std::runtime_error("FAH source factory returned no source");
        _source->start();
    } catch (const std::exception& exception) {
        fah::SourceEvent failure;
        failure.kind = fah::SourceEvent::Kind::Error;
        failure.text = exception.what();
        [self receiveSourceEvent:failure];
    }
    [self updateStatus];
}

- (void)animateOneFrame {
    if (!self.isAnimating) return;
    ++_animationCallbackCount;
    [_metalView draw];
    [self updateStatus];
}

- (void)stopAnimation {
    ++_sourceGeneration;
    if (_source) { _source->stop(); _source.reset(); }
    fah::SourceEvent reset;
    reset.kind = fah::SourceEvent::Kind::Reset;
    _model.receive(reset, _options.loop);
    if (_cache) {
        try { _cache->flush(); }
        catch (const std::exception& exception) { cacheError(exception.what()); }
    }
    [_renderer stop];
    [_renderer setCapture:nullptr];
    [super stopAnimation];
}

- (void)dealloc {
    ++_sourceGeneration;
    if (_source) _source->stop();
    [_renderer stop];
}
- (BOOL)hasConfigureSheet { return YES; }

- (NSWindow *)configureSheet {
    if (_sheet) return _sheet;
    _sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,580,330)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    _sheet.releasedWhenClosed = NO;
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
    label(@"Source URL",280); _sourceURL = text([self sourceURL],280);
    _sourceURL.accessibilityLabel = @"Folding@home source URL";
    label(@"WU ID (optional)",245); _unit = text([_defaults stringForKey:@"unit"],245);
    _unit.accessibilityLabel = @"Optional work unit ID";
    label(@"Snapshots",210); _playback = popup(@[@"Follow latest", @"Loop available frames"],210);
    _playback.frame = NSMakeRect(170,210,200,26);
    [_playback selectItemAtIndex:[_defaults boolForKey:@"loop"] ? 1 : 0];
    _playback.target = self; _playback.action = @selector(updatePlaybackControls:);
    _snapshotInterval = text([[_defaults objectForKey:@"snapshotInterval"] stringValue],210);
    _snapshotInterval.frame = NSMakeRect(380,210,65,24);
    _snapshotInterval.accessibilityLabel = @"Seconds between snapshots";
    _snapshotInterval.toolTip = @"0.05–60 seconds between snapshots. This does not change the display frame rate.";
    _snapshotIntervalUnit = [NSTextField labelWithString:@"seconds/frame"];
    _snapshotIntervalUnit.frame = NSMakeRect(450,210,110,24); [content addSubview:_snapshotIntervalUnit];
    [self updatePlaybackControls:nil];
    label(@"Model style",175); _style = popup(@[@"Space-filling",@"Ball-and-stick",@"Sticks",@"Cycle styles",@"Random styles"],175);
    [_style selectItemAtIndex:[_defaults integerForKey:@"style"]];
    label(@"Frame-rate limit",140); _fps = text([[_defaults objectForKey:@"fps"] stringValue],140);
    _fps.frame = NSMakeRect(170,140,90,24);
    _fps.accessibilityLabel = @"Frame-rate limit in frames per second";
    NSTextField *unitLabel = [NSTextField labelWithString:@"fps (1–240)"];
    unitLabel.frame = NSMakeRect(270,140,140,24); [content addSubview:unitLabel];
    label(@"Background",105); _background = popup(@[@"Light sky and grid",@"Dark sky and grid",@"Solid colour"],105);
    [_background selectItemAtIndex:![_defaults boolForKey:@"sky"] ? 2 : ([_defaults boolForKey:@"darkSky"] ? 1 : 0)];
    _colour = [[NSColorWell alloc] initWithFrame:NSMakeRect(430,105,65,26)];
    _colour.color = [NSColor colorWithSRGBRed:[_defaults floatForKey:@"red"]
        green:[_defaults floatForKey:@"green"] blue:[_defaults floatForKey:@"blue"] alpha:1];
    _playback.accessibilityLabel = @"Snapshot playback";
    _style.accessibilityLabel = @"Model style";
    _background.accessibilityLabel = @"Background";
    _colour.accessibilityLabel = @"Solid background colour";
    [content addSubview:_colour];
    NSBundle *module = [NSBundle bundleForClass:FAHSaverView.class];
    NSString *version = [module objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    NSString *build = [module objectForInfoDictionaryKey:@"CFBundleVersion"];
    NSTextField *versionLabel = [NSTextField labelWithString:version.length && build.length ?
        [NSString stringWithFormat:@"Version %@ (%@)",version,build] : @"Version unavailable"];
    versionLabel.frame = NSMakeRect(20,24,300,20);
    versionLabel.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    versionLabel.textColor = NSColor.secondaryLabelColor;
    versionLabel.accessibilityLabel = @"Screensaver version";
    [content addSubview:versionLabel];
    NSButton *save = [NSButton buttonWithTitle:@"Save" target:self action:@selector(saveOptions:)];
    save.frame = NSMakeRect(460,20,95,32); save.keyEquivalent = @"\r"; [content addSubview:save];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancelOptions:)];
    cancel.frame = NSMakeRect(355,20,95,32); cancel.keyEquivalent = @"\x1b"; [content addSubview:cancel];
    return _sheet;
}

- (void)updatePlaybackControls:(id)sender {
    const BOOL loop = _playback.indexOfSelectedItem == 1;
    _snapshotInterval.hidden = !loop;
    _snapshotInterval.enabled = loop;
    _snapshotIntervalUnit.hidden = !loop;
}

- (void)saveOptions:(id)sender {
    NSScanner *scanner = [NSScanner scannerWithString:_fps.stringValue];
    NSInteger fps;
    BOOL valid = [scanner scanInteger:&fps] && scanner.isAtEnd && fps >= 1 && fps <= 240;
    NSString *problem = valid ? nil : @"Enter a whole frame-rate limit from 1 to 240 fps.";
    double interval = [_defaults doubleForKey:@"snapshotInterval"];
    if (!problem && _playback.indexOfSelectedItem == 1) {
        NSScanner *intervalScanner = [NSScanner scannerWithString:_snapshotInterval.stringValue];
        intervalScanner.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        if (![intervalScanner scanDouble:&interval] || !intervalScanner.isAtEnd ||
            !std::isfinite(interval) || interval < .05 || interval > 60)
            problem = @"Enter a snapshot interval from 0.05 to 60 seconds.";
    }
    if (!problem) {
        NSURL *url = [NSURL URLWithString:_sourceURL.stringValue];
        if (!url.host.length || !([url.scheme isEqualToString:@"ws"] || [url.scheme isEqualToString:@"wss"]))
            problem = @"Enter a WebSocket source URL beginning with ws:// or wss://.";
    }
    if (problem) { NSAlert *alert = [NSAlert new]; alert.messageText = problem; [alert runModal]; return; }
    [_defaults setObject:_sourceURL.stringValue forKey:@"sourceURL"];
    [_defaults setObject:_unit.stringValue forKey:@"unit"];
    [_defaults setBool:_playback.indexOfSelectedItem == 1 forKey:@"loop"];
    [_defaults setDouble:interval forKey:@"snapshotInterval"];
    [_defaults setInteger:_style.indexOfSelectedItem forKey:@"style"];
    [_defaults setInteger:fps forKey:@"fps"];
    [_defaults setBool:_background.indexOfSelectedItem != 2 forKey:@"sky"];
    [_defaults setBool:_background.indexOfSelectedItem == 1 forKey:@"darkSky"];
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
