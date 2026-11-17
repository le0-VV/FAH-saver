#import "FAHSaverView.h"
#import <os/log.h>
#include "fah/LiveModel.hpp"
#include "fah/SnapshotCache.hpp"
#include "fah/DecimalInput.hpp"

#include <memory>
#include <cmath>
#include <string>

static NSString * const FAHDefaultsName = @"org.fahsaver.screensaver";

static BOOL legacySaverHost() {
    return [NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.ScreenSaver.Engine.legacyScreenSaver"];
}

static BOOL frontmostIsLoginWindow() {
    return [NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier isEqualToString:@"com.apple.loginwindow"];
}

@interface FAHDecimalFormatter : NSFormatter
@end
@implementation FAHDecimalFormatter
- (NSString *)stringForObjectValue:(id)value {
    if ([value isKindOfClass:NSString.class]) return value;
    if ([value isKindOfClass:NSNumber.class])
        return [NSString stringWithUTF8String:fah::decimalText([value doubleValue]).c_str()];
    return nil;
}
- (BOOL)getObjectValue:(id *)value forString:(NSString *)text errorDescription:(NSString **)error {
    // Save validates the range; unfinished decimal text must not trigger AppKit's error dialog.
    if (value) *value = text;
    return YES;
}
@end

@interface FAHDecimalEditor : NSTextView
@property(nonatomic, weak) NSTextField *decimalField;
@end
@implementation FAHDecimalEditor
- (BOOL)acceptText:(NSString *)text {
    if (fah::decimalPartial(text.UTF8String ?: "")) return YES;
    id<NSTextFieldDelegate> delegate = self.decimalField.delegate;
    if ([delegate respondsToSelector:@selector(control:didFailToValidatePartialString:errorDescription:)])
        [delegate control:self.decimalField didFailToValidatePartialString:text errorDescription:@"Digits and one decimal point only."];
    return NO;
}
- (BOOL)shouldChangeTextInRange:(NSRange)range replacementString:(NSString *)replacement {
    if (replacement && (range.location > self.string.length || range.length > self.string.length-range.location ||
        ![self acceptText:[self.string stringByReplacingCharactersInRange:range withString:replacement]])) return NO;
    return [super shouldChangeTextInRange:range replacementString:replacement];
}
- (BOOL)shouldChangeTextInRanges:(NSArray<NSValue *> *)ranges replacementStrings:(NSArray<NSString *> *)replacements {
    if (replacements) {
        if (ranges.count != replacements.count) return NO;
        NSMutableString *text = [self.string mutableCopy];
        for (NSInteger index=(NSInteger)ranges.count-1; index>=0; --index) {
            NSRange range = ranges[index].rangeValue;
            if (range.location > text.length || range.length > text.length-range.location) return NO;
            [text replaceCharactersInRange:range withString:replacements[index]];
        }
        if (![self acceptText:text]) return NO;
    }
    return [super shouldChangeTextInRanges:ranges replacementStrings:replacements];
}
@end

@interface FAHDecimalCell : NSTextFieldCell
@end
@implementation FAHDecimalCell {
    FAHDecimalEditor *_decimalEditor;
}
- (NSTextView *)fieldEditorForView:(NSView *)controlView {
    if (!_decimalEditor) {
        _decimalEditor = [[FAHDecimalEditor alloc] initWithFrame:NSZeroRect];
        _decimalEditor.fieldEditor = YES;
        _decimalEditor.richText = NO;
        _decimalEditor.importsGraphics = NO;
    }
    _decimalEditor.decimalField = (NSTextField *)controlView;
    return _decimalEditor;
}
@end

@interface FAHDecimalField : NSTextField
@end
@implementation FAHDecimalField
+ (Class)cellClass { return FAHDecimalCell.class; }
@end

@interface FAHSaverView () <NSTextFieldDelegate>
@end

static NSString *editingText(NSTextField *field) {
    return field.currentEditor ? field.currentEditor.string : field.stringValue;
}

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
    BOOL _resumeWhenPresented, _detachedFromSuperview;
    CGSize _lastDrawableSize;
    __weak NSWindow *_observedWindow;
    id _windowCloseObserver;
    id _workspaceSessionObserver;
    BOOL _sessionGuardEnabled, _sessionArmed, _sessionDismissed, _sessionRestartRequested;
    BOOL _sessionLoginObserved, _sessionSawActivation;
    NSUInteger _sourceGeneration, _sourceCallbackCount, _animationCallbackCount, _measurementCallbackBaseline;
    NSWindow *_sheet;
    NSPopUpButton *_playback, *_style, *_background;
    NSTextField *_sourceURL, *_unit, *_fps, *_snapshotInterval, *_snapshotIntervalUnit, *_optionsError;
    NSMapTable<NSView *,NSValue *> *_optionsBaseFrames;
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
        @"loop": @NO, @"style": @0, @"fps": @24,
        @"sky": @YES, @"darkSky": @NO, @"snapshotInterval": @1,
        @"red": @.02, @"green": @.025, @"blue": @.035 }];
    NSError *error = nil;
    _renderer = [[FAHRenderer alloc] initWithDevice:MTLCreateSystemDefaultDevice() error:&error];
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
    _sessionGuardEnabled = !isPreview && legacySaverHost();
    if (_sessionGuardEnabled) {
        _sessionLoginObserved = frontmostIsLoginWindow();
        __weak FAHSaverView *weakSelf = self;
        _workspaceSessionObserver = [NSWorkspace.sharedWorkspace.notificationCenter
            addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *note) { [weakSelf workspaceApplicationActivated:note]; }];
    }
    return self;
}

- (void)workspaceApplicationActivated:(NSNotification *)notification {
    if (!_sessionGuardEnabled || self.isPreview) return;
    id application = notification.userInfo[NSWorkspaceApplicationKey];
    if (![application respondsToSelector:@selector(bundleIdentifier)]) return;
    _sessionSawActivation = YES;
    _sessionLoginObserved = [[application bundleIdentifier] isEqualToString:@"com.apple.loginwindow"];
    if (_sessionLoginObserved) {
        if (self.isAnimating) _sessionArmed = YES;
        const BOOL restart = _sessionRestartRequested;
        _sessionRestartRequested = NO;
        if (restart) [self startAnimation];
    } else if (_sessionArmed) {
        [self stopAnimation];
    }
}

- (NSString *)sourceURL {
    NSString *url = [_defaults stringForKey:@"sourceURL"];
    // Retired file-only settings can have an empty source; use the normal local endpoint.
    return url.length ? url : @"ws://127.0.0.1:7396/api/websocket";
}

- (fah::SourceOptions)sourceOptions {
    return {self.sourceURL.UTF8String, [[_defaults stringForKey:@"unit"] UTF8String] ?: "", !_options.loop};
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
    const CGSize size = _metalView ? _metalView.drawableSize : _lastDrawableSize;
    result[@"drawable_width"] = @(size.width);
    result[@"drawable_height"] = @(size.height);
    result[@"drawing_mode"] = @"ScreenSaverView animateOneFrame";
    result[@"animation_callbacks"] = @(_animationCallbackCount-_measurementCallbackBaseline);
    result[@"model_available"] = @(_model.available());
    result[@"using_cached_model"] = @(_model.usingFallback());
    result[@"connection_message_visible"] = @(!_status.hidden);
    result[@"cache_worker_active"] = @(_cache != nullptr);
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
    if (self.isAnimating) return;
    if (_sessionGuardEnabled && !self.isPreview) {
        // An explicit workspace exit is authoritative; a cached frontmost query must not undo it.
        if (!_sessionSawActivation && !_sessionDismissed && legacySaverHost() && frontmostIsLoginWindow())
            _sessionLoginObserved = YES;
        if (_sessionDismissed && !_sessionLoginObserved) {
            _sessionRestartRequested = YES;
            return;
        }
    }
    if (self.isHiddenOrHasHiddenAncestor || _detachedFromSuperview) {
        _resumeWhenPresented = YES;
        return;
    }
    _resumeWhenPresented = NO;
    if (!_renderer) { [self updateStatus]; return; }
    [self preparePresentationView];
    [super startAnimation];
    _sessionDismissed = NO;
    if (_sessionGuardEnabled && !self.isPreview && _sessionLoginObserved) _sessionArmed = YES;
    _sessionRestartRequested = NO;
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

- (void)preparePresentationView {
    if (_metalView) return;
    _metalView = [[MTKView alloc] initWithFrame:self.bounds device:_renderer.device];
    _metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _metalView.framebufferOnly = YES;
    _metalView.paused = YES;
    _metalView.enableSetNeedsDisplay = NO;
    _metalView.delegate = _renderer;
    [self addSubview:_metalView positioned:NSWindowBelow relativeTo:_status];
}

- (void)stopAnimationResources {
    ++_sourceGeneration;
    [super stopAnimation];
    if (_source) { _source->stop(); _source.reset(); }
    fah::SourceEvent reset;
    reset.kind = fah::SourceEvent::Kind::Reset;
    _model.receive(reset, _options.loop);
    if (_cache) {
        try { _cache->flush(); }
        catch (const std::exception& exception) { cacheError(exception.what()); }
        _cache.reset();
    }
    [_renderer stop];
    [_renderer setCapture:nullptr];
    if (_metalView) {
        _lastDrawableSize = _metalView.drawableSize;
        _metalView.delegate = nil;
        [_metalView releaseDrawables];
        [_metalView removeFromSuperview];
        _metalView = nil;
    }
}

- (void)stopAnimation {
    _resumeWhenPresented = NO;
    _sessionRestartRequested = NO;
    if (_sessionGuardEnabled && !self.isPreview) {
        _sessionDismissed = YES;
        _sessionArmed = NO;
    }
    [self stopAnimationResources];
}

- (void)suspendAnimationForPresentation:(BOOL)resume {
    [self stopAnimationResources];
    _resumeWhenPresented = resume;
}

- (void)resumeAnimationForPresentation {
    if (_resumeWhenPresented && !_detachedFromSuperview && !self.isHiddenOrHasHiddenAncestor)
        [self startAnimation];
}

- (void)viewDidHide {
    const BOOL resume = self.isAnimating || _resumeWhenPresented;
    [super viewDidHide];
    [self suspendAnimationForPresentation:resume];
}

- (void)viewDidUnhide {
    [super viewDidUnhide];
    [self resumeAnimationForPresentation];
}

- (void)viewWillMoveToSuperview:(NSView *)newSuperview {
    const BOOL detached = self.superview != nil && newSuperview == nil;
    const BOOL resume = self.isAnimating || _resumeWhenPresented;
    if (detached) _detachedFromSuperview = YES;
    [super viewWillMoveToSuperview:newSuperview];
    if (detached) [self suspendAnimationForPresentation:resume];
}

- (void)viewDidMoveToSuperview {
    [super viewDidMoveToSuperview];
    if (self.superview) {
        _detachedFromSuperview = NO;
        [self resumeAnimationForPresentation];
    }
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    NSWindow *window = self.window;
    if (window == _observedWindow && (window || !_windowCloseObserver)) return;
    if (_windowCloseObserver) [NSNotificationCenter.defaultCenter removeObserver:_windowCloseObserver];
    _windowCloseObserver = nil;
    _observedWindow = window;
    if (window) {
        __weak FAHSaverView *weakSelf = self;
        _windowCloseObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowWillCloseNotification
            object:window queue:nil usingBlock:^(NSNotification *note) { [weakSelf stopAnimation]; }];
    }
}

- (void)dealloc {
    if (_workspaceSessionObserver) [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:_workspaceSessionObserver];
    if (_windowCloseObserver) [NSNotificationCenter.defaultCenter removeObserver:_windowCloseObserver];
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
    _optionsBaseFrames = [NSMapTable strongToStrongObjectsMapTable];
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
    _sourceURL.delegate = self;
    label(@"WU ID (optional)",245); _unit = text([_defaults stringForKey:@"unit"],245);
    _unit.accessibilityLabel = @"Optional work unit ID";
    label(@"Snapshots",210); _playback = popup(@[@"Follow latest", @"Loop available frames"],210);
    _playback.frame = NSMakeRect(170,210,200,26);
    [_playback selectItemAtIndex:[_defaults boolForKey:@"loop"] ? 1 : 0];
    _playback.target = self; _playback.action = @selector(updatePlaybackControls:);
    const double savedInterval = [_defaults doubleForKey:@"snapshotInterval"];
    _snapshotInterval = [[FAHDecimalField alloc] initWithFrame:NSMakeRect(380,210,65,24)];
    _snapshotInterval.stringValue = std::isfinite(savedInterval) && savedInterval >= 0 ?
        [NSString stringWithUTF8String:fah::decimalText(savedInterval).c_str()] : @"";
    [content addSubview:_snapshotInterval];
    _snapshotInterval.accessibilityLabel = @"Seconds between snapshots";
    _snapshotInterval.toolTip = @"At least 0.05 seconds between snapshots. This does not change the display frame rate.";
    _snapshotInterval.formatter = [FAHDecimalFormatter new];
    _snapshotInterval.delegate = self;
    _snapshotInterval.cell.scrollable = YES;
    _snapshotIntervalUnit = [NSTextField labelWithString:@"seconds/frame"];
    _snapshotIntervalUnit.frame = NSMakeRect(450,210,110,24); [content addSubview:_snapshotIntervalUnit];
    [self updatePlaybackControls:nil];
    label(@"Model style",175); _style = popup(@[@"Space-filling",@"Ball-and-stick",@"Sticks",@"Cycle styles",@"Random styles"],175);
    [_style selectItemAtIndex:[_defaults integerForKey:@"style"]];
    label(@"Frame-rate limit",140); _fps = text([[_defaults objectForKey:@"fps"] stringValue],140);
    _fps.frame = NSMakeRect(170,140,90,24);
    _fps.accessibilityLabel = @"Frame-rate limit in frames per second";
    _fps.delegate = self;
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
    _optionsError = [NSTextField wrappingLabelWithString:@""];
    _optionsError.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _optionsError.textColor = NSColor.systemRedColor;
    _optionsError.accessibilityLabel = @"Options validation error";
    _optionsError.hidden = YES;
    [content addSubview:_optionsError];
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
    for (NSView *control in content.subviews)
        if (control != _optionsError && control.frame.origin.y >= 100)
            [_optionsBaseFrames setObject:[NSValue valueWithRect:control.frame] forKey:control];
    if (!std::isfinite(savedInterval) || savedInterval < .05)
        [self showOptionsError:@"Enter at least 0.05 seconds." field:_snapshotInterval];
    return _sheet;
}

- (void)updatePlaybackControls:(id)sender {
    const BOOL loop = _playback.indexOfSelectedItem == 1;
    _snapshotInterval.hidden = !loop;
    _snapshotInterval.enabled = loop;
    _snapshotIntervalUnit.hidden = !loop;
    if (!loop && _snapshotInterval.currentEditor) [_sheet makeFirstResponder:_playback];
    [self clearOptionsError];
}

- (void)clearOptionsError {
    for (NSView *control in _optionsBaseFrames.keyEnumerator)
        control.frame = [_optionsBaseFrames objectForKey:control].rectValue;
    _optionsError.stringValue = @"";
    _optionsError.hidden = YES;
}

- (void)showOptionsError:(NSString *)message field:(NSTextField *)field {
    [self clearOptionsError];
    const NSRect fieldFrame = [_optionsBaseFrames objectForKey:field].rectValue;
    for (NSView *control in _optionsBaseFrames.keyEnumerator) {
        NSRect frame = [_optionsBaseFrames objectForKey:control].rectValue;
        if (frame.origin.y < fieldFrame.origin.y) { frame.origin.y -= 18; control.frame = frame; }
    }
    _optionsError.frame = NSMakeRect(fieldFrame.origin.x,fieldFrame.origin.y-18,
        _sheet.contentView.bounds.size.width-fieldFrame.origin.x-20,14);
    _optionsError.stringValue = message;
    _optionsError.hidden = NO;
    if (!field.currentEditor || field.currentEditor != _sheet.firstResponder) [_sheet makeFirstResponder:field];
}

- (void)controlTextDidChange:(NSNotification *)notification { [self clearOptionsError]; }
- (void)control:(NSControl *)control didFailToValidatePartialString:(NSString *)text errorDescription:(NSString *)error {
    if (control == _snapshotInterval) [self showOptionsError:@"Digits and one decimal point only." field:_snapshotInterval];
}

- (void)saveOptions:(id)sender {
    NSScanner *scanner = [NSScanner scannerWithString:editingText(_fps)];
    NSInteger fps;
    BOOL valid = [scanner scanInteger:&fps] && scanner.isAtEnd && fps >= 1 && fps <= 240;
    NSString *problem = valid ? nil : @"Enter a whole number from 1 to 240.";
    NSTextField *problemField = _fps;
    double interval = [_defaults doubleForKey:@"snapshotInterval"];
    if (!problem && _playback.indexOfSelectedItem == 1) {
        NSString *intervalText = editingText(_snapshotInterval);
        NSScanner *intervalScanner = [NSScanner scannerWithString:intervalText];
        intervalScanner.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        if (!fah::decimalPartial(intervalText.UTF8String ?: "")) problem = @"Digits and one decimal point only.";
        else if (![intervalScanner scanDouble:&interval] || !intervalScanner.isAtEnd || !std::isfinite(interval))
            problem = intervalText.length && ![intervalText isEqualToString:@"."] ? @"Enter a finite number." : @"Enter at least 0.05 seconds.";
        else if (interval < .05) problem = @"Enter at least 0.05 seconds.";
        if (problem) {
            problemField = _snapshotInterval;
        }
    }
    if (!problem) {
        NSURL *url = [NSURL URLWithString:editingText(_sourceURL)];
        if (!url.host.length || !([url.scheme isEqualToString:@"ws"] || [url.scheme isEqualToString:@"wss"])) {
            problem = @"Enter a ws:// or wss:// URL.";
            problemField = _sourceURL;
        }
    }
    if (problem) { [self showOptionsError:problem field:problemField]; return; }
    [self clearOptionsError];
    [_defaults setObject:editingText(_sourceURL) forKey:@"sourceURL"];
    [_defaults setObject:editingText(_unit) forKey:@"unit"];
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
