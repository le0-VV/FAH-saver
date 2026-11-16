#include "fah/SnapshotCache.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid snapshot cache operation was accepted");
}
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("fah-snapshot-cache-test-" + std::to_string(tick));
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("Could not create temporary test directory");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), data.size());
    check(bool(output), "Could not write test file");
}
const std::string selector = std::string("ws://PRIVATE_SOURCE:7396/api/websocket") + '\0' + "PRIVATE_WU";
std::shared_ptr<const fah::Topology> topology() {
    return std::make_shared<const fah::Topology>(fah::parseTopology(
        R"({"atoms":[["C"],["O"]],"bonds":[[0,1]]})"));
}
std::shared_ptr<const fah::Frame> frame(float value) {
    return std::make_shared<const fah::Frame>(fah::Frame{{value, 0, 0}, {0, 1, 0}});
}
void checkSnapshot(const std::shared_ptr<const fah::Capture>& snapshot, float value) {
    check(snapshot && snapshot->frames.size() == 1 && snapshot->topology.atoms.size() == 2 &&
          snapshot->topology.bonds.size() == 1 && snapshot->frames[0][0].x == value,
          "Cache did not retain the expected complete latest snapshot");
}
} // namespace

int main() {
    const char* stage = "initial persistence";
    try {
        TemporaryDirectory temporary;
        const auto file = temporary.path / "nested" / "latest.snapshot";
        {
            fah::SnapshotCache cache(file, selector);
            check(!cache.load(), "Missing cache must return no snapshot");
            rejects([&] { cache.queue(nullptr, frame(1)); });
            rejects([&] { cache.queue(topology(), std::make_shared<const fah::Frame>()); });
            cache.queue(topology(), frame(1));
            cache.flush();
            checkSnapshot(cache.load(), 1);
            cache.queue(topology(), frame(2));
            // Destruction must finish the pending valid snapshot.
        }
        fah::SnapshotCache cache(file, selector);
        stage = "restart and source isolation";
        checkSnapshot(cache.load(), 2);
        const auto valid = bytes(file);
        check(valid.find("PRIVATE_SOURCE") == std::string::npos && valid.find("PRIVATE_WU") == std::string::npos,
              "Cache stored plaintext source information");
        fah::SnapshotCache differentSource(file, selector + "different");
        check(!differentSource.load(), "Cache leaked a model across source selectors");
        fah::SnapshotCache differentUnit(file, std::string("ws://PRIVATE_SOURCE:7396/api/websocket") + '\0' + "OTHER_WU");
        check(!differentUnit.load(), "Cache leaked a model across requested WUs");
#ifndef _WIN32
        struct stat attributes{};
        check(!stat(file.c_str(), &attributes) && (attributes.st_mode & 0777) == 0600,
              "Snapshot cache permissions are not private");
#endif
        stage = "malformed cache rejection";
        for (const auto& corrupt : {std::string("bad header"), valid.substr(0, valid.size() / 2)}) {
            write(file, corrupt);
            rejects([&] { cache.load(); });
        }
        write(file, valid);
        const auto headerEnd = valid.find('\n', valid.find('\n') + 1) + 1;
        write(file, valid.substr(0, headerEnd) + "{PRIVATE_CACHE_PAYLOAD");
        try { cache.load(); throw std::logic_error("Corrupt cache was accepted"); }
        catch (const std::runtime_error& error) {
            check(std::string(error.what()).find("PRIVATE_CACHE_PAYLOAD") == std::string::npos,
                  "Corrupt cache contents leaked into diagnostics");
        }
        const auto payload = fah::parseCapture(valid.substr(headerEnd));
        auto multiple = payload;
        multiple.frames.push_back(payload.frames[0]);
        write(file, valid.substr(0, headerEnd) + fah::writeCapture(multiple));
        rejects([&] { cache.load(); });
        write(file, valid);
        std::filesystem::resize_file(file, 32 * 1024 * 1024 + 1024);
        rejects([&] { cache.load(); });
        write(file, valid);

        stage = "invalid write preservation and recovery";
        cache.queue(topology(), frame(std::numeric_limits<float>::quiet_NaN()));
        rejects([&] { cache.flush(); });
        check(bytes(file) == valid, "Invalid data replaced the previous valid snapshot");
        checkSnapshot(cache.load(), 2);
        cache.queue(topology(), frame(3));
        cache.flush();
        checkSnapshot(cache.load(), 3);
        const auto beforeIOFailure = bytes(file);
        stage = "read-only destination failure";
#ifdef _WIN32
        check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY), "Could not make the test cache read-only");
#else
        check(!chmod(file.parent_path().c_str(), 0500), "Could not make the test directory read-only");
#endif
        cache.queue(topology(), frame(4));
        bool failed = false;
        try { cache.flush(); } catch (const std::runtime_error&) { failed = true; }
#ifdef _WIN32
        stage = "restore writable destination";
        check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL),
              "Could not restore writable test cache attributes");
#else
        chmod(file.parent_path().c_str(), 0700);
#endif
        check(failed, "IO failure was not reported");
        check(bytes(file) == beforeIOFailure, "Failed write changed the previous valid cache");
        checkSnapshot(cache.load(), 3);
        for (const auto& entry : std::filesystem::directory_iterator(file.parent_path()))
            check(entry.path() == file, "Failed write left a temporary file behind");

        stage = "concurrent reader and atomic replacement";
        std::atomic<bool> reading{true};
        std::atomic<unsigned> readCount{0};
        std::exception_ptr readFailure;
        std::thread reader([&] {
            try {
                do {
                    const auto snapshot = cache.load();
                    check(snapshot && snapshot->frames.size() == 1 && snapshot->frames[0].size() == 2,
                          "Concurrent reader observed a partial snapshot");
                    ++readCount;
                } while (reading);
            } catch (...) { readFailure = std::current_exception(); }
        });
        try {
            for (unsigned i = 0; i < 30; ++i) {
                cache.queue(topology(), frame(float(i)));
                cache.flush();
            }
        } catch (...) { reading = false; reader.join(); throw; }
        reading = false;
        reader.join();
        if (readFailure) std::rethrow_exception(readFailure);
        check(readCount > 1, "Concurrent cache reader did not run");
        checkSnapshot(cache.load(), 29);

        stage = "queued latest snapshot coalescing";
        std::mutex mutex;
        std::condition_variable condition;
        bool callbackEntered = false, releaseCallback = false;
        std::atomic<unsigned> callbacks{0};
        const auto mainThread = std::this_thread::get_id();
        bool handlerOnWorker = false;
        const auto coalescedFile = temporary.path / "coalesced.snapshot";
        {
            fah::SnapshotCache coalesced(coalescedFile, selector, [&](const std::string& message) {
                std::unique_lock<std::mutex> lock(mutex);
                check(!message.empty(), "Write failure diagnostic is empty");
                handlerOnWorker = std::this_thread::get_id() != mainThread;
                ++callbacks;
                callbackEntered = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releaseCallback; });
            });
            coalesced.queue(topology(), frame(std::numeric_limits<float>::quiet_NaN()));
            {
                std::unique_lock<std::mutex> lock(mutex);
                check(condition.wait_for(lock, std::chrono::seconds(3), [&] { return callbackEntered; }),
                      "Worker did not report its write failure");
            }
            auto superseded = frame(1);
            std::weak_ptr<const fah::Frame> weak = superseded;
            coalesced.queue(topology(), superseded);
            superseded.reset();
            for (unsigned i = 2; i <= 1000; ++i) coalesced.queue(topology(), frame(float(i)));
            const bool pendingWasReleased = weak.expired();
            {
                std::lock_guard<std::mutex> lock(mutex);
                releaseCallback = true;
            }
            condition.notify_all();
            coalesced.flush();
            check(pendingWasReleased, "Superseded queued snapshots remained retained");
            checkSnapshot(coalesced.load(), 1000);
        }
        check(callbacks == 1 && handlerOnWorker, "Write error callbacks did not run safely on the worker");
        std::cout << "Snapshot cache persistence, isolation, validation, atomic replacement and coalescing passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Snapshot cache stage '" << stage << "': " << error.what() << '\n';
        return 1;
    }
}
