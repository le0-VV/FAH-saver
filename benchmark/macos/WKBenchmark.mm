#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <sys/resource.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static double cpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

@interface Benchmark : NSObject <NSApplicationDelegate, WKScriptMessageHandler, WKNavigationDelegate>
@property NSWindow *window;
@property WKWebView *web;
@property NSDictionary<NSString *, NSString *> *options;
@property double started;
@property double initialCPU;
@property BOOL finished;
- (void)fail:(NSString *)message;
@end

@implementation Benchmark
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.started = NSProcessInfo.processInfo.systemUptime;
    self.initialCPU = cpuSeconds();
    NSError *error;
    NSData *capture = [NSData dataWithContentsOfFile:self.options[@"capture"]
                                           options:NSDataReadingMappedIfSafe error:&error];
    if (!capture || capture.length > 50 * 1024 * 1024) {
        [self fail:capture ? @"Capture exceeds 50 MiB" : error.localizedDescription];
        return;
    }
    id object = [NSJSONSerialization JSONObjectWithData:capture options:0 error:&error];
    if (![object isKindOfClass:NSDictionary.class]) {
        [self fail:@"Capture must be a JSON object"];
        return;
    }
    NSData *safeJSON = [NSJSONSerialization dataWithJSONObject:object options:0 error:&error];
    NSString *literal = [[NSString alloc] initWithData:safeJSON encoding:NSUTF8StringEncoding];
    if (!literal) { [self fail:error.localizedDescription ?: @"Invalid capture encoding"]; return; }
    WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    [configuration.userContentController addScriptMessageHandler:self name:@"benchmark"];
    NSString *injection = [@"window.nativeCapture=" stringByAppendingString:literal];
    [configuration.userContentController addUserScript:[[WKUserScript alloc]
        initWithSource:injection injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
    const CGFloat scale = NSScreen.mainScreen.backingScaleFactor;
    NSRect bounds = NSMakeRect(0, 0, self.options[@"width"].doubleValue / scale,
                              self.options[@"height"].doubleValue / scale);
    self.web = [[WKWebView alloc] initWithFrame:bounds configuration:configuration];
    self.web.navigationDelegate = self;
    self.window = [[NSWindow alloc] initWithContentRect:bounds
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"FAH WebGL comparison";
    self.window.contentView = self.web;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    NSURL *html = [NSURL fileURLWithPath:self.options[@"html"]];
    NSURLComponents *url = [NSURLComponents componentsWithURL:html resolvingAgainstBaseURL:NO];
    NSMutableArray *query = [NSMutableArray array];
    for (NSString *key in @[@"style", @"fps", @"seconds", @"background", @"playback"])
        [query addObject:[NSURLQueryItem queryItemWithName:key value:self.options[key]]];
    [query addObject:[NSURLQueryItem queryItemWithName:@"benchmark" value:@"1"]];
    url.queryItems = query;
    [self.web loadFileURL:url.URL allowingReadAccessToURL:[html URLByDeletingLastPathComponent]];
    __weak Benchmark *weakSelf = self;
    [NSTimer scheduledTimerWithTimeInterval:self.options[@"seconds"].doubleValue + 30 repeats:NO
        block:^(NSTimer *timer) {
            (void)timer;
            if (!weakSelf.finished) [weakSelf fail:@"WebGL comparison timed out"];
        }];
}

- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller;
    if (self.finished) return;
    if (![message.body isKindOfClass:NSDictionary.class]) { [self fail:@"Invalid benchmark result"]; return; }
    NSMutableDictionary *result = [message.body mutableCopy];
    if (![result[@"status"] isEqual:@"ok"]) {
        [self fail:result[@"error"] ?: @"WebGL renderer reported a failure"];
        return;
    }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    result[@"host_cpu_seconds_including_startup"] = @(cpuSeconds() - self.initialCPU);
    result[@"host_elapsed_seconds_including_startup"] = @(NSProcessInfo.processInfo.systemUptime - self.started);
    result[@"host_peak_resident_bytes"] = @(usage.ru_maxrss);
    result[@"web_process_cpu_and_memory"] = @"unmeasured";
    result[@"web_gpu_process_cpu_and_memory"] = @"unmeasured";
    result[@"power_watts"] = @"unmeasured";
    result[@"host"] = @"WKWebView";
    result[@"complete_process_cost_measured"] = @NO;
    result[@"drawable_width"] = @(self.web.bounds.size.width * self.window.backingScaleFactor);
    result[@"drawable_height"] = @(self.web.bounds.size.height * self.window.backingScaleFactor);
    NSError *error;
    NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:&error];
    if (!json) { [self fail:error.localizedDescription]; return; }
    NSString *output = self.options[@"output"];
    if (output && ![json writeToFile:output options:NSDataWritingAtomic error:&error]) {
        [self fail:error.localizedDescription]; return;
    }
    fwrite(json.bytes, 1, json.length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    self.finished = YES;
    NSString *screenshot = self.options[@"screenshot"];
    if (!screenshot) { [self finish]; return; }
    [self.web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image, NSError *snapshotError) {
        if (!image) { self.finished = NO; [self fail:snapshotError.localizedDescription]; return; }
        NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:image.TIFFRepresentation];
        NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        NSError *writeError;
        if (!png || ![png writeToFile:screenshot options:NSDataWritingAtomic error:&writeError]) {
            self.finished = NO;
            [self fail:writeError.localizedDescription ?: @"Cannot encode screenshot"];
            return;
        }
        [self finish];
    }];
}

- (void)finish {
    [self.web.configuration.userContentController removeScriptMessageHandlerForName:@"benchmark"];
    [NSApp terminate:nil];
}
- (void)fail:(NSString *)message {
    fprintf(stderr, "WebGL comparison: %s\n", (message ?: @"Unknown error").UTF8String);
    fflush(stderr);
    exit(EXIT_FAILURE);
}
- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)navigation; [self fail:error.localizedDescription];
}
- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)webView; (void)navigation; [self fail:error.localizedDescription];
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    (void)webView; if (!self.finished) [self fail:@"Web content process terminated"];
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSMutableDictionary *options = [@{@"width": @"1280", @"height": @"720", @"fps": @"60",
            @"seconds": @"10", @"style": @"1", @"background": @"sky", @"playback": @"latest"} mutableCopy];
        NSSet *known = [NSSet setWithArray:@[@"capture", @"html", @"width", @"height", @"fps", @"seconds",
            @"style", @"background", @"playback", @"output", @"screenshot"]];
        for (int i = 1; i < argc; i += 2) {
            NSString *argument = @(argv[i]);
            if ([argument isEqual:@"--help"]) {
                puts("Usage: fah-web-benchmark --capture FILE --html index.html [--fps 60] [--seconds 10]\n"
                     "  [--style 1|2|3|cycle|random] [--playback latest|loop] [--background sky|solid]\n"
                     "  [--width 1280] [--height 720] [--output results.json] [--screenshot image.png]");
                return 0;
            }
            NSString *key = [argument hasPrefix:@"--"] ? [argument substringFromIndex:2] : @"";
            if (![known containsObject:key] || i + 1 >= argc) {
                fprintf(stderr, "Invalid or incomplete option: %s\n", argv[i]); return 2;
            }
            options[key] = @(argv[i + 1]);
        }
        if (!options[@"capture"] || !options[@"html"]) { fputs("--capture and --html are required\n", stderr); return 2; }
        for (NSString *key in @[@"width", @"height", @"fps", @"seconds"]) {
            const char *text = [options[key] UTF8String];
            char *end;
            double value = strtod(text, &end);
            double maximum = [key isEqual:@"fps"] ? 240 : ([key isEqual:@"seconds"] ? 3600 : 8192);
            if (*end || !std::isfinite(value) || value < 1 || value > maximum ||
                (![key isEqual:@"seconds"] && std::floor(value) != value)) {
                fprintf(stderr, "Invalid value for --%s\n", key.UTF8String); return 2;
            }
        }
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        Benchmark *delegate = [Benchmark new];
        delegate.options = options;
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
