#pragma once

#include "FAHSaverView.h"

struct TemporaryModelCache {
    std::filesystem::path directory = std::filesystem::path(NSTemporaryDirectory().fileSystemRepresentation) /
        (std::string("fah-saver-test-") + NSUUID.UUID.UUIDString.UTF8String);
    TemporaryModelCache() { std::filesystem::create_directories(directory); }
    ~TemporaryModelCache() { std::error_code error; std::filesystem::remove_all(directory,error); }
    std::filesystem::path file(const char *name = "latest-model.cache") const { return directory/name; }
};

class FixtureSource final : public fah::Source {
    std::shared_ptr<const fah::Capture> capture_;
    fah::SourceCallback callback_;
    bool active_ = false;
public:
    FixtureSource(std::shared_ptr<const fah::Capture> capture, fah::SourceCallback callback) :
        capture_(std::move(capture)), callback_(std::move(callback)) {}
    void start() override {
        active_ = true;
        fah::SourceEvent reset; reset.kind = fah::SourceEvent::Kind::Reset; callback_(std::move(reset));
        fah::SourceEvent topology; topology.kind = fah::SourceEvent::Kind::Topology; topology.unit = "fixture";
        topology.topology = std::shared_ptr<const fah::Topology>(capture_, &capture_->topology);
        callback_(std::move(topology));
        for (std::size_t index=0; active_ && index<capture_->frames.size(); ++index) {
            fah::SourceEvent frame; frame.kind = fah::SourceEvent::Kind::Frame; frame.unit = "fixture"; frame.index = index;
            frame.frame = std::shared_ptr<const fah::Frame>(capture_, &capture_->frames[index]);
            callback_(std::move(frame));
        }
    }
    void stop() override { active_ = false; }
};

inline FAHSaverSourceFactory fixtureSource(std::shared_ptr<const fah::Capture> capture) {
    return [capture=std::move(capture)](fah::SourceOptions, fah::SourceCallback callback) {
        return std::make_unique<FixtureSource>(capture,std::move(callback));
    };
}
