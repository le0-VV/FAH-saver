#pragma once

#include "fah/Model.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace fah {

// Stores only the latest complete topology and snapshot. The selector should be
// source URL + '\0' + requested WU; only its stable identifier is written to disk.
class SnapshotCache {
public:
    using ErrorHandler = std::function<void(const std::string&)>;
    SnapshotCache(std::filesystem::path path, std::string sourceSelector,
                  ErrorHandler onError = {});
    ~SnapshotCache();
    SnapshotCache(const SnapshotCache&) = delete;
    SnapshotCache& operator=(const SnapshotCache&) = delete;

    // Missing or source-mismatched files return nullptr. Invalid files and IO
    // failures throw std::runtime_error. Loading never changes the stored file.
    std::shared_ptr<const Capture> load() const;

    // Keep these objects immutable. Pending snapshots coalesce on one IO worker;
    // onError runs there and must not destroy this cache or call flush().
    void queue(std::shared_ptr<const Topology> topology, std::shared_ptr<const Frame> frame);
    // Wait for work queued before this call; throw the latest outstanding write error.
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fah
