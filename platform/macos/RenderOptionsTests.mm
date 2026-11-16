#import "FAHRenderer.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            check(argc == 2, "Supply the two-snapshot fixture");
            auto capture = std::make_shared<const fah::Capture>(fah::readCapture(argv[1]));
            check(capture->frames.size() == 2, "Timing test requires two snapshots");
            NSError *error = nil;
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            FAHRenderer *renderer = [[FAHRenderer alloc] initWithDevice:device error:&error];
            check(renderer != nil, "Could not initialise the Metal test renderer");
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
            check(texture != nil, "Could not allocate the test texture");
            [renderer setCapture:capture];
            auto draw = [&](double time) {
                id<MTLCommandBuffer> command = [renderer renderToTexture:texture time:time drawable:nil];
                check(command != nil, "Renderer did not submit the test draw");
                [command waitUntilCompleted];
                check(command.status == MTLCommandBufferStatusCompleted, "Metal draw failed");
                return [[renderer metrics] objectForKey:@"snapshot_index"];
            };
            FAHRenderOptions options;
            check(options.snapshotInterval == 1, "Default snapshot interval is not 1 second");
            for (double interval : {1.0, 1.25, 120.0}) {
                options.loop = true; options.snapshotInterval = interval;
                [renderer setOptions:options]; [renderer start];
                for (const auto &sample : {std::pair<double,unsigned>{0,0}, {interval-.001,0},
                                          {interval,1}, {2*interval-.001,1}, {2*interval,0}})
                    check([draw(sample.first) unsignedIntegerValue] == sample.second,
                          "Snapshot loop advanced at the wrong boundary");
                [renderer stop];
            }
            options.loop = false;
            [renderer setOptions:options]; [renderer start];
            check([draw(0) unsignedIntegerValue] == 1 && [draw(20) unsignedIntegerValue] == 1,
                  "Latest mode followed the loop clock");
            [renderer stop];
            for (double interval : {0.0, .049, std::numeric_limits<double>::infinity(),
                                    std::numeric_limits<double>::quiet_NaN()}) {
                options.snapshotInterval = interval;
                bool rejected = false;
                try { [renderer setOptions:options]; } catch (const std::exception&) { rejected = true; }
                check(rejected, "Invalid snapshot interval was accepted");
            }
            for (double interval : {.05, 60.001, 600.0, 1e100, std::numeric_limits<double>::max()}) {
                options.snapshotInterval = interval; [renderer setOptions:options];
            }
            options.sky = true; options.darkSky = false;
            [renderer setOptions:options]; [renderer start]; draw(0); [renderer stop];
            unsigned char light[4], dark[4], solid[4];
            [texture getBytes:light bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
            options.darkSky = true;
            [renderer setOptions:options]; [renderer start]; draw(0); [renderer stop];
            [texture getBytes:dark bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
            for (unsigned channel=0; channel<3; ++channel)
                check(dark[channel] < light[channel], "Dark background is not darker than the light preset");
            options.sky = false; options.colour = {.2f,.3f,.4f};
            [renderer setOptions:options]; [renderer start]; draw(0); [renderer stop];
            [texture getBytes:solid bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
            check(std::abs(int(solid[0])-102) <= 1 && std::abs(int(solid[1])-77) <= 1 &&
                  std::abs(int(solid[2])-51) <= 1 && solid[3] == 255,
                  "Solid background changed with the sky preset");
            std::puts("Metal snapshot cadence, latest mode, timing bounds and background presets passed");
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"Render option tests: %s\n",exception.what());
            return 1;
        }
    }
}
