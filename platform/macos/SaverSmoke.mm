#import <AppKit/AppKit.h>
#import <ScreenSaver/ScreenSaver.h>
#import "FAHSaverView.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            if (argc != 3) throw std::runtime_error("Usage: fah_macos_saver_smoke BUNDLE.saver CAPTURE.json");
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
            auto capture = std::make_shared<const fah::Capture>(fah::readCapture(argv[2]));
            FAHSaverView *view = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:YES];
            if (!view || !view.hasConfigureSheet || !view.configureSheet)
                throw std::runtime_error("Saver view or configuration sheet could not be created");
            for (int cycle=0; cycle<10; ++cycle) {
                [view startAnimation];
                [view setCapture:capture];
                for (int frame=0; frame<3; ++frame) [view animateOneFrame];
                [view stopAnimation];
                if (view.isAnimating) throw std::runtime_error("Saver did not stop animating");
                [view animateOneFrame];
            }
            std::puts("Saver bundle loaded; principal class, configuration sheet and ten start/stop cycles passed.\n"
                "This is a module host smoke check, not a ScreenSaverEngine or lock/sleep test.");
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"FAH saver smoke: %s\n",exception.what());
            return 1;
        }
    }
}
