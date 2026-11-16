#import <AppKit/AppKit.h>
#include "FAHSaverView.h"
#include "TestSource.hpp"
#include "fah/SnapshotCache.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

struct Preferences {
    __strong NSString *name = [@"org.fahsaver.cache-test." stringByAppendingString:NSUUID.UUID.UUIDString];
    __strong NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:name];
    ~Preferences() { [defaults removePersistentDomainForName:name]; }
};

struct Control {
    fah::SourceCallback callback;
    fah::SourceOptions options;
    std::function<void()> onStart;
    unsigned starts = 0;
    void send(fah::SourceEvent event) { check(bool(callback),"Source has not started"); callback(std::move(event)); }
};

class ControlledSource final : public fah::Source {
    std::shared_ptr<Control> control_;
public:
    ControlledSource(std::shared_ptr<Control> control, fah::SourceCallback callback) : control_(std::move(control)) {
        control_->callback = std::move(callback);
    }
    void start() override { ++control_->starts; if (control_->onStart) control_->onStart(); }
    void stop() override {}
};

static FAHSaverSourceFactory source(std::shared_ptr<Control> control) {
    return [control](fah::SourceOptions options, fah::SourceCallback callback) {
        control->options = std::move(options);
        return std::make_unique<ControlledSource>(control,std::move(callback));
    };
}

static fah::SourceEvent event(fah::SourceEvent::Kind kind, const char *unit = "") {
    fah::SourceEvent value; value.kind = kind; value.unit = unit; value.text = "Test source status"; return value;
}

static void expectModel(FAHSaverView *view, std::size_t atoms, std::size_t frames, bool cached) {
    NSDictionary *metrics = [view renderMetrics];
    check([metrics[@"model_available"] boolValue],"Expected model is absent");
    check([metrics[@"atom_count"] unsignedIntegerValue] == atoms &&
          [metrics[@"snapshot_count"] unsignedIntegerValue] == frames,"Displayed topology/snapshot count is wrong");
    check([metrics[@"using_cached_model"] boolValue] == cached,"Wrong live/cache mode");
    check(![metrics[@"connection_message_visible"] boolValue],"Connection text covers a valid model");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        try {
            check(argc == 3,"Supply saver bundle and capture fixture");
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
            NSError *error = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:argv[1]]];
            check(bundle && [bundle loadAndReturnError:&error],"Could not load saver bundle");
            Class principal = bundle.principalClass;
            auto fixture = fah::readCapture(argv[2]);
            check(fixture.frames.size() >= 2 && fixture.topology.atoms.size() > 1,"Fixture is too small");
            TemporaryModelCache cache;
            Preferences preferences;
            const std::string url = "ws://cache-test.invalid:7396/api/websocket";
            std::string selector = url; selector.push_back('\0');
            [preferences.defaults setObject:[NSString stringWithUTF8String:url.c_str()] forKey:@"sourceURL"];
            [preferences.defaults setBool:YES forKey:@"loop"];
            {
                fah::SnapshotCache seed(cache.file(),selector);
                seed.queue(std::make_shared<const fah::Topology>(fixture.topology),
                           std::make_shared<const fah::Frame>(fixture.frames.back()));
                seed.flush();
            }
            auto control = std::make_shared<Control>();
            FAHSaverView *view = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:NO
                preferences:preferences.defaults cachePath:cache.file() sourceFactory:source(control)];
            expectModel(view,fixture.topology.atoms.size(),1,true);
            check(control->starts == 0,"Cache construction started networking");
            __weak FAHSaverView *weakView = view;
            const auto originalAtoms = fixture.topology.atoms.size();
            control->onStart = [weakView,originalAtoms] {
                expectModel(weakView,originalAtoms,1,true);
            };
            [view startAnimation];
            check(control->starts == 1,"Injected source did not start");
            check(!control->options.latestOnly,"Loop mode requested latest-only source data");
            control->send(event(fah::SourceEvent::Kind::Status));
            control->send(event(fah::SourceEvent::Kind::Error));
            control->send(event(fah::SourceEvent::Kind::Reset));
            expectModel(view,fixture.topology.atoms.size(),1,true);
            auto topology = std::make_shared<fah::Topology>(fixture.topology);
            topology->atoms.pop_back(); topology->bonds.clear();
            auto topologyEvent = event(fah::SourceEvent::Kind::Topology,"next");
            topologyEvent.topology = topology; control->send(topologyEvent);
            expectModel(view,fixture.topology.atoms.size(),1,true);
            auto frame = std::make_shared<fah::Frame>(fixture.frames.front()); frame->pop_back();
            auto frameEvent = event(fah::SourceEvent::Kind::Frame,"wrong-unit"); frameEvent.frame = frame;
            bool rejected = false;
            try { control->send(frameEvent); } catch (const std::exception&) { rejected = true; }
            check(rejected,"Mismatched work-unit frame was accepted");
            expectModel(view,fixture.topology.atoms.size(),1,true);
            frameEvent.unit = "next"; control->send(frameEvent);
            expectModel(view,topology->atoms.size(),1,false);
            auto newest = std::make_shared<fah::Frame>(*frame); newest->front().x += .25f;
            frameEvent.frame = newest; control->send(frameEvent);
            expectModel(view,topology->atoms.size(),2,false);
            control->send(event(fah::SourceEvent::Kind::Reset));
            expectModel(view,topology->atoms.size(),1,true);
            control->send(event(fah::SourceEvent::Kind::Error));
            expectModel(view,topology->atoms.size(),1,true);
            [view stopAnimation];
            const auto stoppedCallbacks = view.sourceCallbackCount;
            control->send(event(fah::SourceEvent::Kind::Reset));
            check(view.sourceCallbackCount == stoppedCallbacks && !view.hasActiveSource,"Late source callback was accepted after stop");
            {
                fah::SnapshotCache saved(cache.file(),selector);
                auto recording = saved.load();
                check(recording && recording->frames.size() == 1 && recording->topology.atoms.size() == topology->atoms.size() &&
                      recording->frames.front().front().x == newest->front().x,"Latest complete model was not persisted");
            }
            control->onStart = {};
            [preferences.defaults setBool:NO forKey:@"loop"];
            FAHSaverView *restarted = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:NO
                preferences:preferences.defaults cachePath:cache.file() sourceFactory:source(control)];
            expectModel(restarted,topology->atoms.size(),1,true);
            [restarted startAnimation];
            check(control->options.latestOnly,"Follow latest did not suppress the source's published history");
            expectModel(restarted,topology->atoms.size(),1,true);
            [restarted stopAnimation];
            [preferences.defaults setObject:@"another-work-unit" forKey:@"unit"];
            [restarted startAnimation];
            check(![[restarted renderMetrics][@"model_available"] boolValue],"Cache from another source selection was displayed");
            [restarted stopAnimation];
            [preferences.defaults setObject:@"" forKey:@"unit"];
            const auto badPath = cache.file("corrupt.cache");
            { std::ofstream bad(badPath); bad << "truncated cache"; }
            FAHSaverView *corrupt = [[principal alloc] initWithFrame:NSMakeRect(0,0,320,240) isPreview:NO
                preferences:preferences.defaults cachePath:badPath sourceFactory:source(control)];
            check(![[corrupt renderMetrics][@"model_available"] boolValue],"Corrupt cache was displayed");
            [corrupt startAnimation];
            control->send(topologyEvent); control->send(frameEvent);
            expectModel(corrupt,topology->atoms.size(),1,false);
            [corrupt stopAnimation];
            fah::SnapshotCache repaired(badPath,selector);
            check(repaired.load() != nullptr,"Live reception did not replace a corrupt cache");
            std::puts("Mac cached startup, disconnect, WU transition, live handover, latest-only restart, source isolation and corrupt-cache recovery passed");
            return 0;
        } catch (const std::exception& exception) {
            std::fprintf(stderr,"Mac cache lifecycle: %s\n",exception.what());
            return 1;
        }
    }
}
