#import "FAHRenderer.h"
#import <QuartzCore/QuartzCore.h>
#include "fah/Source.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

static void pumpUntil(double deadline) {
    while (CACurrentMediaTime() < deadline) {
        @autoreleasepool {
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:std::min(.01,deadline-CACurrentMediaTime())]];
        }
    }
}

static double number(const std::string& value, const char *name) {
    std::size_t end = 0;
    double parsed;
    try { parsed = std::stod(value, &end); }
    catch (...) { throw std::runtime_error(std::string("Invalid ")+name); }
    if (end != value.size() || !std::isfinite(parsed)) throw std::runtime_error(std::string("Invalid ")+name);
    return parsed;
}

static void savePNG(id<MTLTexture> texture, NSString *path) {
    const NSUInteger width = texture.width, height = texture.height;
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil
        pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
        isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
        bytesPerRow:width*4 bitsPerPixel:32];
    if (!bitmap) throw std::runtime_error("Could not allocate the screenshot bitmap");
    [texture getBytes:bitmap.bitmapData bytesPerRow:width*4
        fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
    for (NSUInteger i=0; i<width*height; ++i) std::swap(bitmap.bitmapData[i*4],bitmap.bitmapData[i*4+2]);
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSError *error = nil;
    if (!png || ![png writeToFile:path options:NSDataWritingAtomic error:&error])
        throw std::runtime_error(error ? error.localizedDescription.UTF8String : "Could not encode screenshot");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            std::string capturePath, sourceURL, unit, screenshotPath;
            FAHRenderOptions options;
            options.collectMetrics = true;
            int width = 1024, height = 768;
            double seconds = 10;
            bool offscreen = false;
            for (int i=1; i<argc; ++i) {
                const std::string flag = argv[i];
                if (flag == "--help") {
                    std::puts("FAH Metal preview/benchmark\n"
                        "  --capture PATH | --source ws://HOST:7396/api/websocket [--unit ID]\n"
                        "  --style 1|2|3|cycle|random --playback latest|loop --fps 1..240\n"
                        "  --background sky|solid --colour R,G,B --seconds N\n"
                        "  --width PIXELS --height PIXELS --offscreen --screenshot PATH\n"
                        "Offscreen mode reports GPU submissions, not displayed frame rate.\n"
                        "Screenshots require --offscreen. No screensaver is installed or activated.");
                    return 0;
                }
                if (flag == "--offscreen") { offscreen = true; continue; }
                if (i+1 == argc) throw std::runtime_error("Missing value for "+flag);
                const std::string value = argv[++i];
                if (flag == "--capture") capturePath = value;
                else if (flag == "--source") sourceURL = value;
                else if (flag == "--unit") unit = value;
                else if (flag == "--screenshot") screenshotPath = value;
                else if (flag == "--seconds") seconds = number(value,"duration");
                else if (flag == "--fps") {
                    const double fps = number(value,"frame-rate limit");
                    if (fps != std::floor(fps) || fps < 1 || fps > 240)
                        throw std::runtime_error("Frame-rate limit must be a whole number from 1 to 240");
                    options.fps = static_cast<int>(fps);
                } else if (flag == "--width" || flag == "--height") {
                    const double size = number(value,"drawable size");
                    if (size != std::floor(size) || size < 64 || size > 4096)
                        throw std::runtime_error("Drawable dimensions must be whole numbers from 64 to 4096");
                    (flag == "--width" ? width : height) = static_cast<int>(size);
                } else if (flag == "--style") {
                    if (value == "cycle") options.styleMode = FAHStyleMode::Cycle;
                    else if (value == "random") options.styleMode = FAHStyleMode::Random;
                    else if (value == "1" || value == "2" || value == "3")
                        options.style = static_cast<fah::Style>(value[0]-'0');
                    else throw std::runtime_error("Style must be 1, 2, 3, cycle or random");
                } else if (flag == "--playback") {
                    if (value != "latest" && value != "loop") throw std::runtime_error("Playback must be latest or loop");
                    options.loop = value == "loop";
                } else if (flag == "--background") {
                    if (value != "sky" && value != "solid") throw std::runtime_error("Background must be sky or solid");
                    options.sky = value == "sky";
                } else if (flag == "--colour") {
                    const auto a = value.find(','), b = a == std::string::npos ? a : value.find(',',a+1);
                    if (a == std::string::npos || b == std::string::npos || value.find(',',b+1) != std::string::npos)
                        throw std::runtime_error("Colour must contain R,G,B components from zero to one");
                    options.colour = {static_cast<float>(number(value.substr(0,a),"red")),
                        static_cast<float>(number(value.substr(a+1,b-a-1),"green")),
                        static_cast<float>(number(value.substr(b+1),"blue"))};
                } else throw std::runtime_error("Unknown option: "+flag);
            }
            if (capturePath.empty() == sourceURL.empty()) throw std::runtime_error("Choose exactly one of --capture or --source");
            if (seconds <= 0 || seconds > 3600) throw std::runtime_error("Duration must be greater than zero and at most 3600 seconds");
            if (!offscreen && !screenshotPath.empty()) throw std::runtime_error("--screenshot requires --offscreen");
            if (sourceURL.empty() && !unit.empty()) throw std::runtime_error("--unit requires --source");
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            NSError *error = nil;
            FAHRenderer *renderer = [[FAHRenderer alloc] initWithDevice:MTLCreateSystemDefaultDevice() error:&error];
            if (!renderer) throw std::runtime_error(error ? error.localizedDescription.UTF8String : "Could not initialise Metal");
            [renderer setOptions:options];
            std::shared_ptr<fah::Capture> liveCapture;
            std::unique_ptr<fah::Source> source;
            NSString *fatalError = nil;
            NSString *statusText = @"Connecting to Folding@home…";
            bool ready = false;
            if (!capturePath.empty()) {
                auto capture = std::make_shared<const fah::Capture>(fah::readCapture(capturePath));
                ready = !capture->frames.empty();
                [renderer setCapture:std::move(capture)];
            } else {
                source = fah::makeSource({sourceURL,unit}, [&](const fah::SourceEvent& event) {
                    switch (event.kind) {
                        case fah::SourceEvent::Kind::Reset:
                            ready = false; liveCapture = std::make_shared<fah::Capture>();
                            [renderer setCapture:liveCapture]; break;
                        case fah::SourceEvent::Kind::Topology:
                            liveCapture = std::make_shared<fah::Capture>();
                            liveCapture->topology = *event.topology;
                            [renderer setCapture:liveCapture]; break;
                        case fah::SourceEvent::Kind::Frame:
                            if (!liveCapture || !event.frame) break;
                            if (!options.loop) liveCapture->frames.clear();
                            liveCapture->frames.push_back(*event.frame);
                            ready = true; statusText = nil;
                            [renderer setCapture:liveCapture]; break;
                        case fah::SourceEvent::Kind::Status:
                            statusText = [NSString stringWithUTF8String:event.text.c_str()]; break;
                        case fah::SourceEvent::Kind::Error:
                            fatalError = [NSString stringWithUTF8String:event.text.c_str()]; break;
                    }
                });
                source->start();
                const double deadline = CACurrentMediaTime()+30;
                while (!ready && !fatalError && CACurrentMediaTime() < deadline) pumpUntil(CACurrentMediaTime()+.02);
                if (!ready || fatalError) {
                    source->stop();
                    throw std::runtime_error(fatalError ? fatalError.UTF8String : "Timed out waiting for protein snapshots");
                }
            }
            if (!ready) throw std::runtime_error("Capture contains no protein snapshots");
            [renderer start];
            NSWindow *window = nil;
            MTKView *view = nil;
            NSTextField *statusLabel = nil;
            id<MTLTexture> texture = nil;
            if (offscreen) {
                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                    MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
                descriptor.usage = MTLTextureUsageRenderTarget;
                descriptor.storageMode = MTLStorageModeShared;
                texture = [renderer.device newTextureWithDescriptor:descriptor];
                if (!texture) throw std::runtime_error("Could not allocate offscreen target");
            } else {
                const CGFloat scale = NSScreen.mainScreen.backingScaleFactor;
                window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,width/scale,height/scale)
                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered defer:NO];
                window.title = @"FAH Metal preview";
                window.releasedWhenClosed = NO;
                view = [[MTKView alloc] initWithFrame:window.contentView.bounds device:renderer.device];
                view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
                view.autoResizeDrawable = NO; view.drawableSize = CGSizeMake(width,height);
                view.preferredFramesPerSecond = options.fps; view.delegate = renderer;
                window.contentView = view;
                statusLabel = [NSTextField wrappingLabelWithString:@""];
                statusLabel.frame = NSMakeRect(20,20,width/scale-40,45);
                statusLabel.textColor = NSColor.whiteColor;
                statusLabel.drawsBackground = YES;
                statusLabel.backgroundColor = [NSColor colorWithWhite:0 alpha:.75];
                [view addSubview:statusLabel];
                [window center]; [window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
            }
            const double began = CACurrentMediaTime();
            std::size_t submitted = 0;
            id<MTLCommandBuffer> lastCommand = nil;
            while (CACurrentMediaTime()-began < seconds && (offscreen || window.isVisible)) {
                @autoreleasepool {
                    if (fatalError) break;
                    if (offscreen) {
                        lastCommand = [renderer renderToTexture:texture time:CACurrentMediaTime() drawable:nil];
                        [lastCommand waitUntilCompleted];
                        ++submitted;
                        pumpUntil(began+submitted/static_cast<double>(options.fps));
                    } else {
                        NSString *displayStatus = statusText ? statusText : renderer.lastError;
                        if (![statusLabel.stringValue isEqualToString:displayStatus])
                            statusLabel.stringValue = displayStatus ? displayStatus : @"";
                        pumpUntil(std::min(began+seconds,CACurrentMediaTime()+.02));
                    }
                    if (renderer.lastError) { fatalError = renderer.lastError; break; }
                }
            }
            const double renderedSeconds = CACurrentMediaTime()-began;
            view.paused = YES;
            if (source) source->stop();
            [renderer stop];
            pumpUntil(CACurrentMediaTime()+.05);
            if (fatalError) throw std::runtime_error(fatalError.UTF8String);
            if (lastCommand.status == MTLCommandBufferStatusError)
                throw std::runtime_error(lastCommand.error.localizedDescription.UTF8String);
            if (!screenshotPath.empty()) savePNG(texture,[NSString stringWithUTF8String:screenshotPath.c_str()]);
            NSMutableDictionary *metrics = [[renderer metrics] mutableCopy];
            metrics[@"mode"] = offscreen ? @"offscreen" : @"window";
            metrics[@"drawable_width"] = @(width); metrics[@"drawable_height"] = @(height);
            metrics[@"requested_seconds"] = @(seconds);
            metrics[@"elapsed_seconds"] = @(renderedSeconds);
            metrics[@"background"] = options.sky ? @"sky" : @"solid";
            metrics[@"playback"] = options.loop ? @"loop" : @"latest";
            metrics[@"style"] = options.styleMode == FAHStyleMode::Fixed ?
                [NSString stringWithFormat:@"%d",static_cast<int>(options.style)] :
                options.styleMode == FAHStyleMode::Cycle ? @"cycle" : @"random";
            NSData *json = [NSJSONSerialization dataWithJSONObject:metrics options:NSJSONWritingPrettyPrinted error:&error];
            if (!json) throw std::runtime_error(error.localizedDescription.UTF8String);
            std::fwrite(json.bytes,1,json.length,stdout); std::fputc('\n',stdout);
            [window close];
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"FAH Metal: %s\n",exception.what());
            return 1;
        }
    }
}
