#include "fah/SnapshotCache.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fah {
namespace {
constexpr std::size_t maxPayloadBytes = 32 * 1024 * 1024;
const std::string magic = "FAHSaverSnapshot1\n";
constexpr std::size_t keyBytes = 16;
std::atomic<std::uint64_t> nextTemporary{0};

std::runtime_error ioError(const char* operation, unsigned code) {
    return std::runtime_error(std::string("Snapshot cache ") + operation +
                              " failed (OS error " + std::to_string(code) + ")");
}

std::string identifier(const std::string& selector) {
    // This binds a cache to its source; it is not an authentication mechanism.
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : selector) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(keyBytes) << hash;
    return output.str();
}

std::optional<std::string> readFile(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return std::nullopt;
        throw ioError("open", code);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const DWORD code = GetLastError(); CloseHandle(file); throw ioError("size check", code);
    }
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(maxPayloadBytes + magic.size() + keyBytes + 1)) {
        CloseHandle(file); throw std::runtime_error("Snapshot cache file size is outside the supported limits");
    }
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD received = 0;
    const bool ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &received, nullptr);
    const DWORD code = GetLastError();
    CloseHandle(file);
    if (!ok) throw ioError("read", code);
    if (received != data.size()) throw std::runtime_error("Snapshot cache file is truncated");
#else
    const int file = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (file < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw ioError("open", errno);
    }
    std::string data;
    try {
        struct stat attributes{};
        if (fstat(file, &attributes)) throw ioError("size check", errno);
        if (!S_ISREG(attributes.st_mode) || attributes.st_size <= 0 ||
            attributes.st_size > static_cast<off_t>(maxPayloadBytes + magic.size() + keyBytes + 1))
            throw std::runtime_error("Snapshot cache file type or size is outside the supported limits");
        data.resize(static_cast<std::size_t>(attributes.st_size));
        std::size_t received = 0;
        while (received < data.size()) {
            const auto count = read(file, data.data() + received, data.size() - received);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw ioError("read", errno);
            if (!count) throw std::runtime_error("Snapshot cache file is truncated");
            received += static_cast<std::size_t>(count);
        }
        close(file);
    } catch (...) {
        close(file);
        throw;
    }
#endif
    return data;
}

void atomicWrite(const std::filesystem::path& path, const std::string& data) {
    if (!path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) throw ioError("directory creation", error.value());
    }
    std::filesystem::path temporary;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = path;
        temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                     std::to_wstring(nextTemporary.fetch_add(1));
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) throw ioError("temporary file creation", code);
    }
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Snapshot cache temporary file names are exhausted");
    try {
        DWORD written = 0;
        if (!WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr))
            throw ioError("write", GetLastError());
        if (written != data.size()) throw std::runtime_error("Snapshot cache write was incomplete");
        if (!FlushFileBuffers(file)) throw ioError("flush", GetLastError());
        if (!CloseHandle(file)) { file = INVALID_HANDLE_VALUE; throw ioError("close", GetLastError()); }
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw ioError("replacement", GetLastError());
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(temporary.c_str());
        throw;
    }
#else
    int file = -1;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = path;
        temporary += ".tmp." + std::to_string(getpid()) + "." +
                     std::to_string(nextTemporary.fetch_add(1));
        file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (file >= 0) break;
        if (errno != EEXIST) throw ioError("temporary file creation", errno);
    }
    if (file < 0) throw std::runtime_error("Snapshot cache temporary file names are exhausted");
    try {
        std::size_t written = 0;
        while (written < data.size()) {
            const auto count = write(file, data.data() + written, data.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ioError("write", errno);
            written += static_cast<std::size_t>(count);
        }
        if (fsync(file)) throw ioError("flush", errno);
        if (close(file)) { file = -1; throw ioError("close", errno); }
        file = -1;
        if (rename(temporary.c_str(), path.c_str())) throw ioError("replacement", errno);
    } catch (...) {
        if (file >= 0) close(file);
        unlink(temporary.c_str());
        throw;
    }
#endif
}
} // namespace

struct SnapshotCache::Impl {
    struct Pending {
        std::shared_ptr<const Topology> topology;
        std::shared_ptr<const Frame> frame;
        std::uint64_t sequence;
    };
    const std::filesystem::path path;
    const std::string key;
    ErrorHandler onError;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Pending> pending;
    std::uint64_t queued = 0, completed = 0;
    bool stopping = false;
    std::string error;
    std::thread worker;

    Impl(std::filesystem::path value, std::string selector, ErrorHandler handler)
        : path(std::move(value)), key(identifier(selector)), onError(std::move(handler)) {
        if (path.empty() || !path.has_filename()) throw std::runtime_error("Snapshot cache needs a file path");
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        condition.notify_one();
        worker.join();
    }

    void run() {
        for (;;) {
            Pending job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (!pending) return;
                job = std::move(*pending);
                pending.reset();
            }
            std::string failure;
            try {
                Capture capture{*job.topology, {*job.frame}};
                atomicWrite(path, magic + key + "\n" + writeCapture(capture));
            } catch (const std::exception& exception) {
                failure = exception.what();
            }
            if (!failure.empty() && onError) {
                try { onError(failure); }
                catch (...) { failure += "; error handler failed"; }
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                completed = job.sequence;
                error = std::move(failure);
            }
            condition.notify_all();
        }
    }
};

SnapshotCache::SnapshotCache(std::filesystem::path path, std::string selector, ErrorHandler onError)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(selector), std::move(onError))) {}
SnapshotCache::~SnapshotCache() = default;

std::shared_ptr<const Capture> SnapshotCache::load() const {
    const auto data = readFile(impl_->path);
    if (!data) return nullptr;
    const std::size_t header = magic.size() + keyBytes + 1;
    if (data->size() <= header || data->compare(0, magic.size(), magic) != 0 || (*data)[header - 1] != '\n')
        throw std::runtime_error("Snapshot cache header is invalid");
    const auto key = data->substr(magic.size(), keyBytes);
    if (key.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Snapshot cache source identifier is invalid");
    if (key != impl_->key) return nullptr;
    std::shared_ptr<Capture> capture;
    try { capture = std::make_shared<Capture>(parseCapture(std::string_view(*data).substr(header))); }
    catch (const std::exception&) { throw std::runtime_error("Snapshot cache model data is invalid"); }
    if (capture->frames.size() != 1) throw std::runtime_error("Snapshot cache must contain exactly one frame");
    return capture;
}

void SnapshotCache::queue(std::shared_ptr<const Topology> topology, std::shared_ptr<const Frame> frame) {
    if (!topology || !frame || topology->atoms.empty() || topology->atoms.size() > 200000 ||
        topology->bonds.size() > 1000000 || frame->size() != topology->atoms.size())
        throw std::runtime_error("Snapshot cache needs a complete topology and matching frame");
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pending = Impl::Pending{std::move(topology), std::move(frame), ++impl_->queued};
    }
    impl_->condition.notify_one();
}

void SnapshotCache::flush() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto sequence = impl_->queued;
    impl_->condition.wait(lock, [&] { return impl_->completed >= sequence; });
    if (!impl_->error.empty()) throw std::runtime_error(impl_->error);
}

} // namespace fah
