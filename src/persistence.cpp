// Persistent Execution Fabric - durable file store.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/persistence.hpp"

#include <atomic>
#include <cstring>
#include <limits>
#include <system_error>

#include "pef/hash.hpp"
#include "pef/version.hpp"

#if defined(_WIN32)
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
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pef {
namespace {

constexpr char kJournalMagic[8] = {'P', 'E', 'F', 'J', 'R', 'N', '0', '1'};
constexpr char kSnapshotMagic[8] = {'P', 'E', 'F', 'S', 'N', 'A', 'P', '1'};
constexpr std::uint32_t kRecordMagic = 0x50454652u;  // 'PEFR'
constexpr std::size_t kJournalHeaderSize = 40;
constexpr std::size_t kSnapshotHeaderSize = 56;
constexpr std::size_t kRecordHeaderSize = 40;
constexpr std::size_t kRecordTrailerSize = 8;
constexpr const char* kJournalName = "pef.journal";
constexpr const char* kSnapshotName = "pef.snapshot";
constexpr const char* kSnapshotTempName = "pef.snapshot.tmp";

[[nodiscard]] std::uint64_t read_le64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | p[i];
    }
    return value;
}

[[nodiscard]] std::uint32_t read_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] std::uint16_t read_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

void write_le64(std::uint8_t* p, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
    }
}

void write_le32(std::uint8_t* p, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
    }
}

void write_le16(std::uint8_t* p, std::uint16_t value) noexcept {
    for (int i = 0; i < 2; ++i) {
        p[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
    }
}

template <class Table>
void write_table(ByteWriter& w, const Table& table) {
    const auto order = table.canonical_order();
    w.u32(static_cast<std::uint32_t>(order.size()));
    for (const auto* record : order) {
        encode_record(w, *record);
    }
}

template <class Table>
bool read_table(ByteReader& r, Table& table) {
    const std::uint32_t count = r.u32();
    if (!r.ok()) {
        return false;
    }
    if (count > r.limits().max_container) {
        r.fail();
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        typename Table::value_type record{};
        if (!decode_record(r, record)) {
            return false;
        }
        table.upsert(record);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Platform file primitives.
// ---------------------------------------------------------------------------
class NativeFile {
public:
    NativeFile() = default;
    ~NativeFile() { close(); }
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    [[nodiscard]] Status open_append(const std::filesystem::path& path);
    [[nodiscard]] Status open_truncate(const std::filesystem::path& path);
    [[nodiscard]] Status seek(std::uint64_t offset);
    [[nodiscard]] Status write(const void* data, std::size_t size);
    [[nodiscard]] Status flush();
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalid; }

private:
#if defined(_WIN32)
    static constexpr void* kInvalid = nullptr;
    void* handle_ = kInvalid;
#else
    static constexpr int kInvalid = -1;
    int handle_ = kInvalid;
#endif
};

#if defined(_WIN32)

Status NativeFile::open_append(const std::filesystem::path& path) {
    close();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return err(Code::PersistenceFailure,
                   "cannot open journal for append: " + path.string());
    }
    LARGE_INTEGER zero{};
    zero.QuadPart = 0;
    if (::SetFilePointerEx(h, zero, nullptr, FILE_END) == 0) {
        ::CloseHandle(h);
        return err(Code::PersistenceFailure, "cannot seek journal to end: " + path.string());
    }
    handle_ = h;
    return ok_status();
}

Status NativeFile::seek(std::uint64_t offset) {
    if (!is_open()) {
        return err(Code::PersistenceFailure, "seek on closed file");
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(static_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN) == 0) {
        return err(Code::PersistenceFailure, "SetFilePointerEx failed");
    }
    return ok_status();
}

Status NativeFile::open_truncate(const std::filesystem::path& path) {
    close();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ,
                             FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return err(Code::PersistenceFailure, "cannot create file: " + path.string());
    }
    handle_ = h;
    return ok_status();
}

Status NativeFile::write(const void* data, std::size_t size) {
    if (!is_open()) {
        return err(Code::PersistenceFailure, "write on closed file");
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x10000000u ? 0x10000000u : remaining);
        DWORD written = 0;
        if (::WriteFile(static_cast<HANDLE>(handle_), p, chunk, &written, nullptr) == 0) {
            return err(Code::PersistenceFailure, "WriteFile failed");
        }
        if (written == 0) {
            return err(Code::PersistenceFailure, "WriteFile wrote zero bytes");
        }
        p += written;
        remaining -= written;
    }
    return ok_status();
}

Status NativeFile::flush() {
    if (!is_open()) {
        return err(Code::PersistenceFailure, "flush on closed file");
    }
    if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
        return err(Code::PersistenceFailure, "FlushFileBuffers failed");
    }
    return ok_status();
}

void NativeFile::close() noexcept {
    if (handle_ != kInvalid) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = kInvalid;
    }
}

[[nodiscard]] Status platform_read_file(const std::filesystem::path& path, std::size_t max_bytes,
                                        Bytes& out, bool& exists) {
    exists = false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            return ok_status();
        }
        return err(Code::PersistenceFailure, "cannot open for read: " + path.string());
    }
    exists = true;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(h, &size) == 0) {
        ::CloseHandle(h);
        return err(Code::PersistenceFailure, "GetFileSizeEx failed: " + path.string());
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
        ::CloseHandle(h);
        return err(Code::LimitExceeded, "file exceeds the load limit: " + path.string());
    }
    const auto total = static_cast<std::size_t>(size.QuadPart);
    out.assign(total, std::byte{0});
    std::size_t offset = 0;
    while (offset < total) {
        const DWORD chunk = static_cast<DWORD>(
            (total - offset) > 0x10000000u ? 0x10000000u : (total - offset));
        DWORD got = 0;
        if (::ReadFile(h, out.data() + offset, chunk, &got, nullptr) == 0) {
            ::CloseHandle(h);
            return err(Code::PersistenceFailure, "ReadFile failed: " + path.string());
        }
        if (got == 0) {
            break;
        }
        offset += got;
    }
    ::CloseHandle(h);
    if (offset != total) {
        // Short read: the file shrank underneath us. Report the bytes present.
        out.resize(offset);
    }
    return ok_status();
}

[[nodiscard]] Status platform_atomic_replace(const std::filesystem::path& from,
                                             const std::filesystem::path& to) {
    if (::MoveFileExW(from.c_str(), to.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return err(Code::PersistenceFailure, "MoveFileEx failed for " + to.string());
    }
    return ok_status();
}

[[nodiscard]] Status platform_sync_directory(const std::filesystem::path&) {
    // Windows has no directory fsync. MoveFileExW with MOVEFILE_WRITE_THROUGH
    // is the durable-replace primitive used instead.
    return ok_status();
}

[[nodiscard]] bool platform_exists(const std::filesystem::path& path) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

// Identity generation, not a recovery decision. Store ids are minted once at
// creation and persisted; nothing downstream derives a decision from entropy.
[[nodiscard]] std::uint64_t platform_entropy64() {
    static std::atomic<std::uint64_t> counter{0};
    LARGE_INTEGER qpc{};
    ::QueryPerformanceCounter(&qpc);
    FILETIME created{};
    ::GetSystemTimeAsFileTime(&created);
    const std::uint64_t a = static_cast<std::uint64_t>(qpc.QuadPart);
    const std::uint64_t b = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
                            static_cast<std::uint64_t>(created.dwLowDateTime);
    const std::uint64_t c = static_cast<std::uint64_t>(::GetCurrentProcessId());
    const std::uint64_t d = counter.fetch_add(1, std::memory_order_relaxed);
    HashBuilder h;
    h.u64(a);
    h.u64(b);
    h.u64(c);
    h.u64(d);
    return h.digest();
}

[[nodiscard]] Status platform_remove(const std::filesystem::path& path) {
    if (::DeleteFileW(path.c_str()) == 0) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) {
            return ok_status();
        }
        return err(Code::PersistenceFailure, "DeleteFile failed: " + path.string());
    }
    return ok_status();
}

#else  // POSIX

Status NativeFile::open_append(const std::filesystem::path& path) {
    close();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return err(Code::PersistenceFailure, "cannot open journal for append: " + path.string());
    }
    handle_ = fd;
    return ok_status();
}

Status NativeFile::open_truncate(const std::filesystem::path& path) {
    close();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return err(Code::PersistenceFailure, "cannot create file: " + path.string());
    }
    handle_ = fd;
    return ok_status();
}

Status NativeFile::seek(std::uint64_t offset) {
    if (!is_open()) {
        return err(Code::PersistenceFailure, "seek on closed file");
    }
    if (::lseek(handle_, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return err(Code::PersistenceFailure, "lseek failed");
    }
    return ok_status();
}

Status NativeFile::write(const void* data, std::size_t size) {
    if (!is_open()) {
        return err(Code::PersistenceFailure, "write on closed file");
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t n = ::write(handle_, p, remaining);
        if (n <= 0) {
            return err(Code::PersistenceFailure, "write failed");
        }
        p += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }
    return ok_status();
}

Status NativeFile::flush() {
    if (!is_open()) {
        return err(Code::PersistenceFailure, "flush on closed file");
    }
    if (::fsync(handle_) != 0) {
        return err(Code::PersistenceFailure, "fsync failed");
    }
    return ok_status();
}

void NativeFile::close() noexcept {
    if (handle_ != kInvalid) {
        ::close(handle_);
        handle_ = kInvalid;
    }
}

[[nodiscard]] Status platform_read_file(const std::filesystem::path& path, std::size_t max_bytes,
                                        Bytes& out, bool& exists) {
    exists = false;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return ok_status();
        }
        return err(Code::PersistenceFailure, "cannot open for read: " + path.string());
    }
    exists = true;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return err(Code::PersistenceFailure, "fstat failed: " + path.string());
    }
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > max_bytes) {
        ::close(fd);
        return err(Code::LimitExceeded, "file exceeds the load limit: " + path.string());
    }
    const auto total = static_cast<std::size_t>(st.st_size);
    out.assign(total, std::byte{0});
    std::size_t offset = 0;
    while (offset < total) {
        const ssize_t n = ::read(fd, out.data() + offset, total - offset);
        if (n < 0) {
            ::close(fd);
            return err(Code::PersistenceFailure, "read failed: " + path.string());
        }
        if (n == 0) {
            break;
        }
        offset += static_cast<std::size_t>(n);
    }
    ::close(fd);
    out.resize(offset);
    return ok_status();
}

[[nodiscard]] Status platform_atomic_replace(const std::filesystem::path& from,
                                             const std::filesystem::path& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) {
        return err(Code::PersistenceFailure, "rename failed for " + to.string());
    }
    return ok_status();
}

[[nodiscard]] Status platform_sync_directory(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) {
        return ok_status();
    }
    const int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0 ? ok_status()
                   : err(Code::PersistenceFailure, "directory fsync failed: " + dir.string());
}

[[nodiscard]] bool platform_exists(const std::filesystem::path& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

[[nodiscard]] std::uint64_t platform_entropy64() {
    std::uint64_t value = 0;
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = ::read(fd, &value, sizeof(value));
        ::close(fd);
        if (got != static_cast<ssize_t>(sizeof(value))) {
            value = 0;
        }
    }
    if (value == 0) {
        value = static_cast<std::uint64_t>(::getpid());
    }
    return value;
}

[[nodiscard]] Status platform_remove(const std::filesystem::path& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return err(Code::PersistenceFailure, "unlink failed: " + path.string());
    }
    return ok_status();
}

#endif

// Builds a complete snapshot image.
[[nodiscard]] Bytes build_snapshot_image(const DurableState& state) {
    ByteWriter payload(4096);
    state.encode(payload);
    const Bytes& body = payload.bytes();

    Bytes image(kSnapshotHeaderSize + body.size() + kRecordTrailerSize, std::byte{0});
    std::memcpy(image.data(), kSnapshotMagic, 8);
    write_le32(reinterpret_cast<std::uint8_t*>(image.data()) + 8, kPersistenceSchemaVersion);
    write_le16(reinterpret_cast<std::uint8_t*>(image.data()) + 12, 0);
    write_le16(reinterpret_cast<std::uint8_t*>(image.data()) + 14, 0);
    write_le64(reinterpret_cast<std::uint8_t*>(image.data()) + 16, state.store.value());
    write_le64(reinterpret_cast<std::uint8_t*>(image.data()) + 24, state.sequence);
    write_le64(reinterpret_cast<std::uint8_t*>(image.data()) + 32,
               static_cast<std::uint64_t>(body.size()));
    write_le64(reinterpret_cast<std::uint8_t*>(image.data()) + 40,
               crc64_ecma(body.data(), body.size()));
    write_le64(reinterpret_cast<std::uint8_t*>(image.data()) + 48,
               crc64_ecma(image.data(), 48));
    if (!body.empty()) {
        std::memcpy(image.data() + kSnapshotHeaderSize, body.data(), body.size());
    }
    const std::size_t trailer = kSnapshotHeaderSize + body.size();
    write_le64(reinterpret_cast<std::uint8_t*>(image.data()) + trailer,
               crc64_ecma(image.data(), trailer));
    return image;
}

// Builds a complete record image (header + payload + trailer).
[[nodiscard]] Bytes build_record_image(RecordKind kind, std::uint64_t sequence,
                                       const Bytes& payload) {
    Bytes image(kRecordHeaderSize + payload.size() + kRecordTrailerSize, std::byte{0});
    auto* p = reinterpret_cast<std::uint8_t*>(image.data());
    write_le32(p, kRecordMagic);
    write_le16(p + 4, static_cast<std::uint16_t>(kind));
    write_le16(p + 6, 0);
    write_le64(p + 8, sequence);
    write_le64(p + 16, static_cast<std::uint64_t>(payload.size()));
    write_le64(p + 24, crc64_ecma(payload.data(), payload.size()));
    write_le64(p + 32, crc64_ecma(p, 32));
    if (!payload.empty()) {
        std::memcpy(p + kRecordHeaderSize, payload.data(), payload.size());
    }
    const std::size_t trailer = kRecordHeaderSize + payload.size();
    write_le64(p + trailer, crc64_ecma(p, trailer));
    return image;
}

struct RecordBoundary {
    bool valid = false;
    RecordKind kind = RecordKind::Barrier;
    std::uint64_t sequence = 0;
    std::size_t payload_offset = 0;
    std::size_t payload_size = 0;
    std::size_t total_size = 0;
};

// Attempts to interpret bytes at offset as one record. Every check is
// independent so that a failure identifies the exact reason.
[[nodiscard]] RecordBoundary probe_record(const Bytes& data, std::size_t offset,
                                          const StoreLimits& limits) {
    RecordBoundary out;
    if (offset + kRecordHeaderSize + kRecordTrailerSize > data.size()) {
        return out;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data()) + offset;
    if (read_le32(p) != kRecordMagic) {
        return out;
    }
    if (read_le64(p + 32) != crc64_ecma(p, 32)) {
        return out;
    }
    const std::uint16_t raw_kind = read_le16(p + 4);
    if (raw_kind == 0 || raw_kind > kMaxRecordKind) {
        return out;
    }
    const std::uint64_t declared = read_le64(p + 16);
    if (declared > limits.max_journal_payload) {
        return out;
    }
    const auto payload_size = static_cast<std::size_t>(declared);
    const std::size_t total = kRecordHeaderSize + payload_size + kRecordTrailerSize;
    if (offset + total > data.size()) {
        return out;
    }
    const auto* payload = p + kRecordHeaderSize;
    if (read_le64(p + 24) != crc64_ecma(payload, payload_size)) {
        return out;
    }
    if (read_le64(payload + payload_size) != crc64_ecma(p, kRecordHeaderSize + payload_size)) {
        return out;
    }
    out.valid = true;
    out.kind = static_cast<RecordKind>(raw_kind);
    out.sequence = read_le64(p + 8);
    out.payload_offset = offset + kRecordHeaderSize;
    out.payload_size = payload_size;
    out.total_size = total;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Record kind names.
// ---------------------------------------------------------------------------
std::string_view record_kind_name(RecordKind kind) noexcept {
    switch (kind) {
        case RecordKind::CommitGroup: return "COMMIT_GROUP";
        case RecordKind::StoreMeta: return "STORE_META";
        case RecordKind::Policy: return "POLICY";
        case RecordKind::Execution: return "EXECUTION";
        case RecordKind::Action: return "ACTION";
        case RecordKind::Progress: return "PROGRESS";
        case RecordKind::Checkpoint: return "CHECKPOINT";
        case RecordKind::Continuation: return "CONTINUATION";
        case RecordKind::Lease: return "LEASE";
        case RecordKind::Replay: return "REPLAY";
        case RecordKind::Ambiguity: return "AMBIGUITY";
        case RecordKind::Commit: return "COMMIT";
        case RecordKind::Recovery: return "RECOVERY";
        case RecordKind::Barrier: return "BARRIER";
    }
    return "UNKNOWN_RECORD_KIND";
}

std::optional<RecordKind> parse_record_kind(std::string_view text) noexcept {
    for (std::uint16_t i = 1; i <= kMaxRecordKind; ++i) {
        const auto candidate = static_cast<RecordKind>(i);
        if (record_kind_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Store metadata and barrier records.
// ---------------------------------------------------------------------------
void StoreMetaRecord::encode(ByteWriter& w) const { encode_record(w, *this); }

bool StoreMetaRecord::decode(ByteReader& r, StoreMetaRecord& out) {
    return decode_record(r, out);
}

void BarrierRecord::encode(ByteWriter& w) const { encode_record(w, *this); }

bool BarrierRecord::decode(ByteReader& r, BarrierRecord& out) {
    return decode_record(r, out);
}

// ---------------------------------------------------------------------------
// DurableState.
// ---------------------------------------------------------------------------
void DurableState::clear() {
    policies.clear();
    executions.clear();
    actions.clear();
    progress.clear();
    checkpoints.clear();
    continuations.clear();
    leases.clear();
    replays.clear();
    ambiguities.clear();
    commits.clear();
    recoveries.clear();
}

void DurableState::encode(ByteWriter& w) const {
    w.u32(schema);
    w.u64(store.value());
    w.u64(epoch.value());
    w.u64(sequence);
    w.u64(next_execution_sequence);
    write_table(w, policies);
    write_table(w, executions);
    write_table(w, actions);
    write_table(w, progress);
    write_table(w, checkpoints);
    write_table(w, continuations);
    write_table(w, leases);
    write_table(w, replays);
    write_table(w, ambiguities);
    write_table(w, commits);
    write_table(w, recoveries);
}

bool DurableState::decode(ByteReader& r, DurableState& out) {
    out.clear();
    out.schema = r.u32();
    out.store = StoreId{r.u64()};
    out.epoch = CoordinatorEpoch{r.u64()};
    out.sequence = r.u64();
    out.next_execution_sequence = r.u64();
    if (!r.ok()) {
        return false;
    }
    if (out.schema != kPersistenceSchemaVersion) {
        return false;
    }
    if (!read_table(r, out.policies) || !read_table(r, out.executions) ||
        !read_table(r, out.actions) || !read_table(r, out.progress) ||
        !read_table(r, out.checkpoints) || !read_table(r, out.continuations) ||
        !read_table(r, out.leases) || !read_table(r, out.replays) ||
        !read_table(r, out.ambiguities) || !read_table(r, out.commits) ||
        !read_table(r, out.recoveries)) {
        return false;
    }
    return r.ok();
}

bool apply_record(DurableState& state, RecordKind kind, const Bytes& payload) {
    switch (kind) {
        case RecordKind::StoreMeta: {
            StoreMetaRecord meta;
            ByteReader reader(payload);
            if (!StoreMetaRecord::decode(reader, meta) || !reader.at_end()) {
                return false;
            }
            if (state.store.valid() && meta.store != state.store) {
                return false;
            }
            state.store = meta.store;
            state.epoch = meta.epoch;
            state.next_execution_sequence = meta.next_execution_sequence;
            state.schema = meta.schema;
            return true;
        }
        case RecordKind::Policy: {
            ExecutionPolicy record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.policies.upsert(record);
            return true;
        }
        case RecordKind::Execution: {
            ExecutionRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.executions.upsert(record);
            return true;
        }
        case RecordKind::Action: {
            ActionRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.actions.upsert(record);
            return true;
        }
        case RecordKind::Progress: {
            ProgressRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.progress.upsert(record);
            return true;
        }
        case RecordKind::Checkpoint: {
            CheckpointRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.checkpoints.upsert(record);
            return true;
        }
        case RecordKind::Continuation: {
            ContinuationRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.continuations.upsert(record);
            return true;
        }
        case RecordKind::Lease: {
            LeaseRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.leases.upsert(record);
            return true;
        }
        case RecordKind::Replay: {
            ReplayRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.replays.upsert(record);
            return true;
        }
        case RecordKind::Ambiguity: {
            AmbiguityRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.ambiguities.upsert(record);
            return true;
        }
        case RecordKind::Commit: {
            CommitRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.commits.upsert(record);
            return true;
        }
        case RecordKind::Recovery: {
            RecoveryRecord record{};
            if (!decode_from_bytes(payload, record)) {
                return false;
            }
            state.recoveries.upsert(record);
            return true;
        }
        case RecordKind::Barrier: {
            BarrierRecord record;
            ByteReader reader(payload);
            if (!BarrierRecord::decode(reader, record) || !reader.at_end()) {
                return false;
            }
            return true;
        }
        case RecordKind::CommitGroup: {
            CommitGroupRecord group;
            if (!decode_from_bytes(payload, group)) {
                return false;
            }
            state.commits.upsert(group.commit);
            state.progress.upsert(group.progress);
            state.actions.upsert(group.action);
            state.executions.upsert(group.execution);
            return true;
        }
    }
    return false;
}

bool reconcile_state(DurableState& state, std::vector<CommitGroupRecord>& repairs) {
    bool changed = false;
    // Iterate over a snapshot of the identities: the loop updates records.
    std::vector<ExecutionId> identities;
    for (const ExecutionRecord* execution : state.executions.canonical_order()) {
        identities.push_back(execution->id);
    }
    for (ExecutionId identity : identities) {
        const ExecutionRecord* execution = state.executions.find(identity);
        if (execution == nullptr) {
            continue;
        }
        const ProgressRecord* best = nullptr;
        std::uint64_t committed = 0;
        for (const auto& progress : state.progress.insertion_order()) {
            if (progress.execution != execution->id || !progress.committed) {
                continue;
            }
            ++committed;
            if (best == nullptr || progress.generation > best->generation) {
                best = &progress;
            }
        }
        // An action record that is durable but whose aggregate update was lost
        // still happened: the frontier is brought forward to match.
        std::uint64_t frontier = execution->action_frontier;
        ActionGeneration action_generation = execution->action_generation;
        for (const auto& action : state.actions.insertion_order()) {
            if (action.execution != execution->id) {
                continue;
            }
            if (action.sequence > frontier) {
                frontier = action.sequence;
                action_generation = action.generation;
            } else if (action.sequence == frontier && action.generation > action_generation) {
                action_generation = action.generation;
            }
        }
        if (frontier != execution->action_frontier) {
            ExecutionRecord advanced = *execution;
            advanced.action_frontier = frontier;
            advanced.action_generation = action_generation;
            state.executions.upsert(advanced);
            execution = state.executions.find(identity);
            changed = true;
        }

        if (best == nullptr || best->generation <= execution->progress_generation) {
            continue;
        }
        ExecutionRecord next = *execution;
        next.progress_generation = best->generation;
        next.progress = best->id;
        next.last_commit = best->commit;
        next.committed_actions = committed;

        const CommitRecord* commit = state.commits.find(best->commit);
        if (commit == nullptr) {
            // A committed progress record with no commit identity cannot be
            // promoted; it is left in place and reported by the auditor.
            continue;
        }
        const ActionRecord* action = state.actions.find(ActionKey{commit->action,
                                                                  commit->action_generation});
        if (action == nullptr) {
            continue;
        }
        if (action->status != ActionStatus::Committed || action->commit != commit->id) {
            ActionRecord sealed = *action;
            sealed.status = ActionStatus::Committed;
            sealed.commit = commit->id;
            state.actions.upsert(sealed);
        }
        state.executions.upsert(next);
        CommitGroupRecord repair;
        repair.commit = *commit;
        repair.progress = *best;
        repair.action = *state.actions.find(ActionKey{commit->action, commit->action_generation});
        repair.execution = next;
        repairs.push_back(repair);
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// FileDurableStore.
// ---------------------------------------------------------------------------
struct FileDurableStore::Impl {
    NativeFile journal;
    Bytes write_buffer;
};

FileDurableStore::FileDurableStore() : impl_(std::make_unique<Impl>()) {}

FileDurableStore::~FileDurableStore() { (void)close(); }

Status FileDurableStore::open(const std::filesystem::path& directory, bool create_if_missing,
                              StoreLimits limits) {
    if (open_) {
        return err(Code::InvalidArgument, "store is already open");
    }
    limits_ = limits;
    directory_ = directory;

    std::error_code ec;
    const bool exists = std::filesystem::exists(directory_, ec);
    if (!exists) {
        if (!create_if_missing) {
            return err(Code::NotFound, "store directory does not exist: " + directory_.string());
        }
        std::filesystem::create_directories(directory_, ec);
        if (ec) {
            return err(Code::PersistenceFailure,
                       "cannot create store directory: " + ec.message());
        }
    } else if (!std::filesystem::is_directory(directory_, ec)) {
        return err(Code::InvalidArgument, "store path is not a directory: " + directory_.string());
    }

    const auto journal_path = directory_ / kJournalName;
    const bool journal_exists = platform_exists(journal_path);
    if (!journal_exists) {
        if (!create_if_missing) {
            return err(Code::NotFound, "journal missing in store: " + directory_.string());
        }
        // The store id is minted once, at creation, and persisted in the journal
        // header. It is never derived from the directory path, so relocating a
        // store directory does not silently change durable identity.
        const StoreId fresh_store{platform_entropy64()};
        NativeFile fresh;
        PEF_TRY(fresh.open_truncate(journal_path));
        std::uint8_t header[kJournalHeaderSize] = {};
        std::memcpy(header, kJournalMagic, 8);
        write_le32(header + 8, kPersistenceSchemaVersion);
        write_le16(header + 12, 0);
        write_le16(header + 14, 0);
        write_le64(header + 16, fresh_store.value());
        write_le64(header + 24, 1);
        write_le64(header + 32, crc64_ecma(header, 32));
        PEF_TRY(fresh.write(header, sizeof(header)));
        PEF_TRY(fresh.flush());
        fresh.close();
        PEF_TRY(platform_sync_directory(directory_));
    }

    PEF_TRY(impl_->journal.open_append(journal_path));

    // The journal is read once here: the header establishes the store identity
    // and the first sequence, and the same bytes establish the true append
    // position.
    Bytes journal;
    bool journal_readable = false;
    PEF_TRY(platform_read_file(journal_path, limits_.max_journal_load_bytes, journal,
                               journal_readable));
    if (!journal_readable || journal.size() < kJournalHeaderSize) {
        return err(Code::TruncatedState, "journal header is truncated: " + journal_path.string());
    }
    const auto* h = reinterpret_cast<const std::uint8_t*>(journal.data());
    if (std::memcmp(h, kJournalMagic, 8) != 0) {
        return err(Code::CorruptState, "journal header magic mismatch: " + journal_path.string());
    }
    if (read_le64(h + 32) != crc64_ecma(h, 32)) {
        return err(Code::CorruptState, "journal header checksum mismatch: " + journal_path.string());
    }
    if (read_le32(h + 8) != kPersistenceSchemaVersion) {
        return err(Code::Unsupported, "journal schema version is not supported by this build");
    }
    store_id_ = StoreId{read_le64(h + 16)};
    if (!store_id_.valid()) {
        return err(Code::CorruptState, "journal header carries a nil store id");
    }
    journal_first_sequence_ = read_le64(h + 24);

    // Establish the true append position by scanning the journal for its last
    // readable record. Appending positionally without this would reuse sequence
    // numbers after a crash and silently hide records from replay.
    last_sequence_ = journal_first_sequence_ - 1;
    std::size_t offset = kJournalHeaderSize;
    while (offset < journal.size()) {
        const RecordBoundary boundary = probe_record(journal, offset, limits_);
        if (!boundary.valid) {
            break;
        }
        last_sequence_ = boundary.sequence;
        offset += boundary.total_size;
    }
    // Reposition the append handle past any unreadable tail so new records do
    // not interleave with damaged bytes.
    if (offset != journal.size()) {
        PEF_TRY(impl_->journal.seek(offset));
    }
    open_ = true;
    return ok_status();
}

Status FileDurableStore::append(RecordKind kind, const Bytes& payload, bool flush_now,
                                std::uint64_t& out_sequence) {
    if (!open_) {
        return err(Code::PersistenceFailure, "store is not open");
    }
    if (payload.size() > limits_.max_journal_payload) {
        return err(Code::LimitExceeded, "journal record payload exceeds the configured limit");
    }
    // Sequence numbers are positional and continue from the last record the
    // journal actually contains, which open() established by scanning.
    const std::uint64_t sequence = last_sequence_ + 1;
    const Bytes image = build_record_image(kind, sequence, payload);
    PEF_TRY(impl_->journal.write(image.data(), image.size()));
    if (flush_now) {
        PEF_TRY(impl_->journal.flush());
    }
    last_sequence_ = sequence;
    out_sequence = sequence;
    return ok_status();
}

Status FileDurableStore::flush() {
    if (!open_) {
        return err(Code::PersistenceFailure, "store is not open");
    }
    return impl_->journal.flush();
}

Status FileDurableStore::write_snapshot(const DurableState& state, bool truncate_journal) {
    if (!open_) {
        return err(Code::PersistenceFailure, "store is not open");
    }
    const Bytes image = build_snapshot_image(state);
    if (image.size() > limits_.max_snapshot_payload) {
        return err(Code::LimitExceeded, "snapshot exceeds the configured limit");
    }
    const auto tmp_path = directory_ / kSnapshotTempName;
    const auto final_path = directory_ / kSnapshotName;
    {
        NativeFile tmp;
        PEF_TRY(tmp.open_truncate(tmp_path));
        PEF_TRY(tmp.write(image.data(), image.size()));
        PEF_TRY(tmp.flush());
        tmp.close();
    }
    PEF_TRY(platform_atomic_replace(tmp_path, final_path));
    PEF_TRY(platform_sync_directory(directory_));

    if (truncate_journal) {
        PEF_TRY(impl_->journal.flush());
        impl_->journal.close();
        const auto journal_path = directory_ / kJournalName;
        PEF_TRY(platform_remove(journal_path));
        NativeFile fresh;
        PEF_TRY(fresh.open_truncate(journal_path));
        std::uint8_t header[kJournalHeaderSize] = {};
        std::memcpy(header, kJournalMagic, 8);
        write_le32(header + 8, kPersistenceSchemaVersion);
        write_le16(header + 12, 0);
        write_le16(header + 14, 0);
        write_le64(header + 16, state.store.value());
        write_le64(header + 24, state.sequence + 1);
        write_le64(header + 32, crc64_ecma(header, 32));
        PEF_TRY(fresh.write(header, sizeof(header)));
        PEF_TRY(fresh.flush());
        fresh.close();
        PEF_TRY(platform_sync_directory(directory_));
        PEF_TRY(impl_->journal.open_append(journal_path));
        journal_first_sequence_ = state.sequence + 1;
        last_sequence_ = state.sequence;
    }
    return ok_status();
}

Status FileDurableStore::load(DurableState& out, LoadReport& report) {
    out.clear();
    report = LoadReport{};

    // --- snapshot -----------------------------------------------------------
    Bytes snapshot;
    bool snapshot_exists = false;
    PEF_TRY(platform_read_file(directory_ / kSnapshotName, limits_.max_snapshot_payload, snapshot,
                               snapshot_exists));
    report.snapshot_present = snapshot_exists;
    std::uint64_t snapshot_sequence = 0;
    bool snapshot_usable = false;
    if (snapshot_exists) {
        if (snapshot.size() < kSnapshotHeaderSize + kRecordTrailerSize) {
            report.detail = "snapshot is shorter than its header";
        } else {
            const auto* p = reinterpret_cast<const std::uint8_t*>(snapshot.data());
            const bool magic_ok = std::memcmp(p, kSnapshotMagic, 8) == 0;
            const bool header_ok = read_le64(p + 48) == crc64_ecma(p, 48);
            const std::uint64_t declared = read_le64(p + 32);
            const bool size_ok =
                declared <= limits_.max_snapshot_payload &&
                snapshot.size() == kSnapshotHeaderSize + static_cast<std::size_t>(declared) +
                                       kRecordTrailerSize;
            const bool payload_ok =
                size_ok && read_le64(p + 40) == crc64_ecma(p + kSnapshotHeaderSize,
                                                           static_cast<std::size_t>(declared));
            const bool trailer_ok =
                size_ok && read_le64(p + kSnapshotHeaderSize + static_cast<std::size_t>(declared)) ==
                               crc64_ecma(p, kSnapshotHeaderSize + static_cast<std::size_t>(declared));
            const bool schema_ok = read_le32(p + 8) == kPersistenceSchemaVersion;
            if (magic_ok && header_ok && size_ok && payload_ok && trailer_ok && schema_ok) {
                ByteReader reader(p + kSnapshotHeaderSize, static_cast<std::size_t>(declared));
                DurableState decoded;
                if (DurableState::decode(reader, decoded) && reader.at_end()) {
                    out = std::move(decoded);
                    snapshot_sequence = read_le64(p + 24);
                    out.sequence = snapshot_sequence;
                    snapshot_usable = true;
                } else {
                    report.detail = "snapshot payload does not decode";
                }
            } else if (!magic_ok) {
                report.detail = "snapshot magic mismatch";
            } else if (!header_ok) {
                report.detail = "snapshot header checksum mismatch";
            } else if (!schema_ok) {
                report.detail = "snapshot schema version is not supported by this build";
            } else if (!size_ok) {
                report.detail = "snapshot size does not match its declared payload length";
            } else {
                report.detail = "snapshot payload checksum mismatch";
            }
        }
        report.snapshot_sequence = snapshot_sequence;
        if (!snapshot_usable) {
            // A snapshot that does not verify is refused outright rather than
            // silently ignored: the journal may already have been compacted
            // against it and the recovered state would be a fabrication.
            report.corrupted = true;
            return err(Code::CorruptState, "snapshot failed verification: " + report.detail);
        }
    }

    // --- journal ------------------------------------------------------------
    Bytes journal;
    bool journal_exists = false;
    PEF_TRY(platform_read_file(directory_ / kJournalName, limits_.max_journal_load_bytes, journal,
                               journal_exists));
    report.journal_present = journal_exists;
    if (!journal_exists) {
        if (!snapshot_usable) {
            return err(Code::NotFound, "store has neither snapshot nor journal");
        }
        report.records_loaded = 0;
        return ok_status();
    }
    if (journal.size() < kJournalHeaderSize) {
        return err(Code::TruncatedState, "journal header is truncated");
    }
    const auto* jh = reinterpret_cast<const std::uint8_t*>(journal.data());
    if (std::memcmp(jh, kJournalMagic, 8) != 0) {
        return err(Code::CorruptState, "journal header magic mismatch");
    }
    if (read_le64(jh + 32) != crc64_ecma(jh, 32)) {
        return err(Code::CorruptState, "journal header checksum mismatch");
    }
    if (read_le32(jh + 8) != kPersistenceSchemaVersion) {
        return err(Code::Unsupported, "journal schema version is not supported by this build");
    }
    if (snapshot_usable) {
        const std::uint64_t journal_store = read_le64(jh + 16);
        if (journal_store != 0 && out.store.valid() && journal_store != out.store.value()) {
            return err(Code::CorruptState, "journal store id does not match the snapshot");
        }
    }
    if (out.store.is_nil() && read_le64(jh + 16) != 0) {
        out.store = StoreId{read_le64(jh + 16)};
    }

    std::size_t offset = kJournalHeaderSize;
    while (offset < journal.size()) {
        const RecordBoundary boundary = probe_record(journal, offset, limits_);
        if (!boundary.valid) {
            report.corrupt_offset = offset;
            break;
        }
        if (boundary.sequence > snapshot_sequence) {
            const Bytes payload(
                journal.begin() + static_cast<std::ptrdiff_t>(boundary.payload_offset),
                journal.begin() +
                    static_cast<std::ptrdiff_t>(boundary.payload_offset + boundary.payload_size));
            if (!apply_record(out, boundary.kind, payload)) {
                report.corrupted = true;
                report.corrupt_offset = offset;
                report.detail = "journal record payload does not decode";
                return err(Code::CorruptState, report.detail);
            }
            out.sequence = boundary.sequence;
            ++report.records_loaded;
        } else {
            ++report.records_skipped;
        }
        offset += boundary.total_size;
    }

    if (offset < journal.size()) {
        // Bytes remain after the first unusable boundary. Distinguish a torn
        // tail (the writer died mid-append) from corruption in the middle of a
        // journal that continues afterwards.
        std::size_t scan = offset + 1;
        bool later_valid_record = false;
        while (scan + kRecordHeaderSize + kRecordTrailerSize <= journal.size()) {
            if (probe_record(journal, scan, limits_).valid) {
                later_valid_record = true;
                break;
            }
            ++scan;
        }
        report.bytes_ignored = journal.size() - offset;
        if (later_valid_record) {
            report.corrupted = true;
            report.detail = "journal contains an unreadable record followed by readable records";
            return err(Code::CorruptState, report.detail);
        }
        report.truncated_tail = true;
        if (report.detail.empty()) {
            report.detail = "journal tail is torn or incomplete";
        }
    }

    return ok_status();
}

Status FileDurableStore::close() {
    if (!open_) {
        return ok_status();
    }
    impl_->journal.close();
    open_ = false;
    return ok_status();
}

}  // namespace pef
