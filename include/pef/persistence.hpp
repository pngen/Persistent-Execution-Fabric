// Persistent Execution Fabric - durable store abstraction.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The fabric owns its persistence contract. Storage is a single directory
// holding one atomically-replaced snapshot and one append-only journal. The
// journal is the durability point: a record that has been appended and flushed
// is recoverable by a fresh coordinator.
//
// Every journal record is a full-record upsert. Replay is therefore a pure
// function of the record sequence and is idempotent.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pef/bytes.hpp"
#include "pef/records.hpp"
#include "pef/status.hpp"

namespace pef {

enum class RecordKind : std::uint16_t {
    StoreMeta = 1,
    Policy = 2,
    Execution = 3,
    Action = 4,
    Progress = 5,
    Checkpoint = 6,
    Continuation = 7,
    Lease = 8,
    Replay = 9,
    Ambiguity = 10,
    Commit = 11,
    Recovery = 12,
    // An explicit durability barrier. Carries no state; its presence proves the
    // writer reached a known point.
    Barrier = 13,
};

[[nodiscard]] std::string_view record_kind_name(RecordKind kind) noexcept;
[[nodiscard]] std::optional<RecordKind> parse_record_kind(std::string_view text) noexcept;

inline constexpr std::uint16_t kMaxRecordKind = 13;
[[nodiscard]] constexpr bool pef_valid_enum(RecordKind v) noexcept {
    return static_cast<std::uint16_t>(v) >= 1 && static_cast<std::uint16_t>(v) <= kMaxRecordKind;
}

// Keys used to index each record table.
template <class Rec>
struct RecordKey;

#define PEF_KEY(Rec, IdType, Field)                                     \
    template <>                                                         \
    struct RecordKey<Rec> {                                             \
        using key_type = IdType;                                        \
        static key_type get(const Rec& r) noexcept { return r.Field; }  \
        struct Hash {                                                   \
            std::size_t operator()(const key_type& key) const noexcept { \
                return IdHasher<typename key_type::tag_type>{}(key);    \
            }                                                           \
        };                                                              \
    }

PEF_KEY(ExecutionPolicy, PolicyId, id);
PEF_KEY(ExecutionRecord, ExecutionId, id);
PEF_KEY(ProgressRecord, ProgressId, id);
PEF_KEY(CheckpointRecord, CheckpointId, id);
PEF_KEY(ContinuationRecord, ContinuationId, id);
PEF_KEY(LeaseRecord, LeaseId, id);
PEF_KEY(ReplayRecord, ReplayId, id);
PEF_KEY(AmbiguityRecord, AmbiguityId, id);
PEF_KEY(CommitRecord, CommitId, id);
PEF_KEY(RecoveryRecord, RecoveryId, id);

#undef PEF_KEY

// Actions are keyed by logical identity plus generation.
template <>
struct RecordKey<ActionRecord> {
    using key_type = ActionKey;
    static key_type get(const ActionRecord& r) noexcept {
        return ActionKey{r.id, r.generation};
    }
    struct Hash {
        std::size_t operator()(const ActionKey& key) const noexcept {
            HashBuilder h;
            h << key.id << key.generation;
            return static_cast<std::size_t>(h.digest());
        }
    };
};

// A record table with O(1) expected lookup and stable insertion order. The
// canonical (sorted) materialisation is produced only when bytes are needed.
template <class Rec>
class RecordTable {
public:
    using key_type = typename RecordKey<Rec>::key_type;
    using value_type = Rec;

    [[nodiscard]] Rec* find(key_type id) {
        const auto it = index_.find(id);
        return it == index_.end() ? nullptr : &records_[it->second];
    }
    [[nodiscard]] const Rec* find(key_type id) const {
        const auto it = index_.find(id);
        return it == index_.end() ? nullptr : &records_[it->second];
    }
    [[nodiscard]] bool contains(key_type id) const { return index_.find(id) != index_.end(); }

    // Inserts or replaces. Returns the stored record.
    Rec& upsert(const Rec& record) {
        const key_type id = RecordKey<Rec>::get(record);
        const auto it = index_.find(id);
        if (it != index_.end()) {
            records_[it->second] = record;
            return records_[it->second];
        }
        index_.emplace(id, records_.size());
        records_.push_back(record);
        return records_.back();
    }

    // Erases a record. Used by retention compaction only.
    bool erase(key_type id) {
        const auto it = index_.find(id);
        if (it == index_.end()) {
            return false;
        }
        const std::size_t pos = it->second;
        records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(pos));
        index_.erase(it);
        for (auto& entry : index_) {
            if (entry.second > pos) {
                --entry.second;
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    [[nodiscard]] const std::vector<Rec>& insertion_order() const noexcept { return records_; }
    [[nodiscard]] std::vector<Rec>& insertion_order() noexcept { return records_; }

    void clear() {
        records_.clear();
        index_.clear();
    }

    // Canonical order: ascending by key. Deterministic regardless of the order
    // in which records were inserted.
    [[nodiscard]] std::vector<const Rec*> canonical_order() const {
        std::vector<const Rec*> out;
        out.reserve(records_.size());
        for (const auto& record : records_) {
            out.push_back(&record);
        }
        std::sort(out.begin(), out.end(), [](const Rec* a, const Rec* b) {
            return RecordKey<Rec>::get(*a) < RecordKey<Rec>::get(*b);
        });
        return out;
    }

private:
    std::vector<Rec> records_;
    std::unordered_map<key_type, std::size_t, typename RecordKey<Rec>::Hash> index_;
};

// Durable store metadata. Upserted whenever the derived-identity counter or the
// coordinator epoch advances.
struct StoreMetaRecord {
    StoreId store;
    CoordinatorEpoch epoch;
    // Next sequence value used to derive a deterministic ExecutionId.
    std::uint64_t next_execution_sequence = 1;
    // Journal sequence this metadata was written at.
    std::uint64_t sequence = 0;
    std::uint32_t schema = 0;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, StoreMetaRecord& out);
};

// Barrier record payload.
struct BarrierRecord {
    std::uint64_t sequence = 0;
    std::uint32_t reason = 0;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, BarrierRecord& out);
};

template <>
struct RecordTraits<StoreMetaRecord> {
    static constexpr auto members =
        std::make_tuple(&StoreMetaRecord::store, &StoreMetaRecord::epoch,
                        &StoreMetaRecord::next_execution_sequence, &StoreMetaRecord::sequence,
                        &StoreMetaRecord::schema);
};

template <>
struct RecordTraits<BarrierRecord> {
    static constexpr auto members =
        std::make_tuple(&BarrierRecord::sequence, &BarrierRecord::reason);
};

struct JournalRecord {
    RecordKind kind = RecordKind::Barrier;
    std::uint64_t sequence = 0;
    Bytes payload;
};

// What a load actually observed. Reported so that callers can distinguish a
// clean load from a load that discarded a torn tail or refused corruption.
struct LoadReport {
    bool snapshot_present = false;
    bool journal_present = false;
    bool truncated_tail = false;
    bool corrupted = false;
    std::uint64_t snapshot_sequence = 0;
    std::uint64_t records_loaded = 0;
    std::uint64_t records_skipped = 0;
    std::uint64_t bytes_ignored = 0;
    std::uint64_t corrupt_offset = 0;
    std::string detail;

    [[nodiscard]] bool clean() const noexcept { return !truncated_tail && !corrupted; }
};

// Storage limits. These are persistence limits and are deliberately independent
// of the protocol frame limits.
struct StoreLimits {
    std::size_t max_journal_payload = 8u * 1024u * 1024u;
    std::size_t max_snapshot_payload = 512u * 1024u * 1024u;
    std::size_t max_journal_load_bytes = 1024u * 1024u * 1024u;
};

// The complete durable state of one store.
struct DurableState {
    StoreId store;
    CoordinatorEpoch epoch;
    std::uint64_t sequence = 0;
    std::uint64_t next_execution_sequence = 1;
    std::uint32_t schema = 0;

    RecordTable<ExecutionPolicy> policies;
    RecordTable<ExecutionRecord> executions;
    RecordTable<ActionRecord> actions;
    RecordTable<ProgressRecord> progress;
    RecordTable<CheckpointRecord> checkpoints;
    RecordTable<ContinuationRecord> continuations;
    RecordTable<LeaseRecord> leases;
    RecordTable<ReplayRecord> replays;
    RecordTable<AmbiguityRecord> ambiguities;
    RecordTable<CommitRecord> commits;
    RecordTable<RecoveryRecord> recoveries;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, DurableState& out);
    void clear();

    // Exact action lookup by logical identity and generation.
    [[nodiscard]] const ActionRecord* find_action(ActionId id, ActionGeneration generation) const {
        return actions.find(ActionKey{id, generation});
    }
    // Highest generation recorded for a logical action. Linear in the number of
    // records for that identity only; the table is small per action.
    [[nodiscard]] const ActionRecord* find_latest_action(ActionId id) const {
        const ActionRecord* best = nullptr;
        for (const auto& record : actions.insertion_order()) {
            if (record.id != id) {
                continue;
            }
            if (best == nullptr || record.generation > best->generation) {
                best = &record;
            }
        }
        return best;
    }
};

// Applies one journal record to a state. Upsert semantics: a record with an
// existing identity replaces the stored one. Returns false when the payload
// does not decode, which is a corruption signal.
[[nodiscard]] bool apply_record(DurableState& state, RecordKind kind, const Bytes& payload);

// ---------------------------------------------------------------------------
// FileDurableStore: one directory, one snapshot, one append-only journal.
// ---------------------------------------------------------------------------
class FileDurableStore {
public:
    FileDurableStore();
    ~FileDurableStore();
    FileDurableStore(const FileDurableStore&) = delete;
    FileDurableStore& operator=(const FileDurableStore&) = delete;

    // Opens (and optionally creates) a store directory. Creating a store also
    // writes the initial journal header.
    [[nodiscard]] Status open(const std::filesystem::path& directory, bool create_if_missing,
                              StoreLimits limits);

    // Appends one record. When flush is true the record is on stable storage
    // before this returns.
    [[nodiscard]] Status append(RecordKind kind, const Bytes& payload, bool flush,
                                std::uint64_t& out_sequence);

    // Flushes any buffered bytes to stable storage.
    [[nodiscard]] Status flush();

    // Replaces the snapshot atomically and optionally starts a fresh journal.
    [[nodiscard]] Status write_snapshot(const DurableState& state, bool truncate_journal);

    // Loads the snapshot (if any) and replays journal records newer than it.
    [[nodiscard]] Status load(DurableState& out, LoadReport& report);

    [[nodiscard]] Status close();

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] const StoreLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] StoreId store_id() const noexcept { return store_id_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path directory_;
    StoreLimits limits_{};
    std::uint64_t last_sequence_ = 0;
    std::uint64_t journal_first_sequence_ = 1;
    StoreId store_id_;
    bool open_ = false;
};

}  // namespace pef
