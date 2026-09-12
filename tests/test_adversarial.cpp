// Persistent Execution Fabric - adversarial input and stale-authority tests.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pef/audit.hpp"
#include "pef/protocol.hpp"
#include "pef/runtime.hpp"
#include "test_framework.hpp"

using namespace pef;

namespace {

class Harness {
public:
    explicit Harness(const char* label) : dir_(label) {}

    [[nodiscard]] Status open(bool create = true) {
        RuntimeConfig config;
        config.store_path = dir_.path();
        config.create_if_missing = create;
        const Status status = runtime_.open(config, outcome_);
        caller_.epoch = runtime_.epoch();
        return status;
    }

    [[nodiscard]] Status open_fresh(Runtime& other, bool create = false) {
        RuntimeConfig config;
        config.store_path = dir_.path();
        config.create_if_missing = create;
        OpenOutcome outcome;
        return other.open(config, outcome);
    }

    [[nodiscard]] Status create_execution() {
        CreateExecutionRequest request;
        request.request = RequestId{1};
        request.policy.id = PolicyId{1};
        request.policy.generation = PolicyGeneration{1};
        CreateExecutionResult result;
        PEF_TRY(runtime_.create_execution(caller_, request, result));
        execution_ = result.execution;
        generation_ = result.generation;
        policy_generation_ = result.policy_generation;
        return ok_status();
    }

    [[nodiscard]] Status bind() {
        worker_ = derive_worker_id("adversarial-worker");
        boot_ = mint_worker_boot_id(worker_);
        BindWorkerRequest request;
        request.request = RequestId{2};
        request.execution = execution_;
        request.worker = worker_;
        request.boot = boot_;
        BindWorkerResult result;
        PEF_TRY(runtime_.bind_worker(caller_, request, result));
        token_.execution = execution_;
        token_.execution_generation = generation_;
        token_.incarnation = result.incarnation;
        token_.incarnation_generation = result.incarnation_generation;
        token_.lease = result.lease;
        token_.lease_generation = result.lease_generation;
        token_.epoch = runtime_.epoch();
        token_.worker = worker_;
        token_.boot = boot_;
        token_.policy_generation = policy_generation_;
        return ok_status();
    }

    [[nodiscard]] Status start() {
        StartRequest request;
        request.request = RequestId{3};
        request.token = token_;
        return runtime_.start(request);
    }

    [[nodiscard]] Status step(std::uint64_t index, SideEffectClass cls,
                              CompleteActionResult& out) {
        BeginActionRequest begin;
        begin.request = RequestId{100 + index * 2};
        begin.token = token_;
        begin.effect_class = cls;
        begin.request_key = cls == SideEffectClass::RepeatableWithKey ? "key-" + std::to_string(index)
                                                                     : std::string{};
        BeginActionResult action;
        PEF_TRY(runtime_.begin_action(begin, action));
        CompleteActionRequest complete;
        complete.request = RequestId{101 + index * 2};
        complete.token = token_;
        complete.action = action.action;
        complete.action_generation = action.action_generation;
        complete.completion.effect_applied = true;
        complete.completion.evidence.kind = EvidenceKind::Real;
        complete.completion.evidence.source = "adversarial";
        return runtime_.complete_action(complete, out);
    }

    [[nodiscard]] Status checkpoint(CheckpointId expected, CheckpointGeneration expected_generation,
                                    RegisterCheckpointResult& out) {
        RegisterCheckpointRequest request;
        request.request = RequestId{9000 + expected_generation.value()};
        request.token = token_;
        request.expected_checkpoint = expected;
        request.expected_checkpoint_generation = expected_generation;
        request.content_size = 512;
        request.content_hash = 0x1122334455667788ULL;
        return runtime_.register_checkpoint(request, out);
    }

    void corrupt(const char* name, std::uint64_t offset, std::uint8_t value) {
        const auto path = dir_.path() / name;
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(static_cast<std::streamoff>(offset));
        char existing = 0;
        stream.read(&existing, 1);
        stream.clear();
        // Flip rather than overwrite, so the byte always changes.
        const char byte = static_cast<char>(static_cast<std::uint8_t>(existing) ^ value);
        stream.seekp(static_cast<std::streamoff>(offset));
        stream.write(&byte, 1);
        stream.flush();
    }

    void truncate_file(const char* name, std::uint64_t size) {
        const auto path = dir_.path() / name;
        std::error_code ec;
        std::filesystem::resize_file(path, size, ec);
    }

    [[nodiscard]] std::uint64_t journal_size() const {
        std::error_code ec;
        return std::filesystem::file_size(dir_.path() / "pef.journal", ec);
    }

    [[nodiscard]] Runtime& runtime() { return runtime_; }
    [[nodiscard]] CallerContext& caller() { return caller_; }
    [[nodiscard]] ExecutionId execution() const { return execution_; }
    [[nodiscard]] LeaseToken& token() { return token_; }
    [[nodiscard]] const LeaseToken& token() const { return token_; }
    [[nodiscard]] WorkerId worker() const { return worker_; }
    [[nodiscard]] WorkerBootId boot() const { return boot_; }
    [[nodiscard]] peftest::TempStore& dir() { return dir_; }
    [[nodiscard]] OpenOutcome& outcome() { return outcome_; }

private:
    peftest::TempStore dir_;
    Runtime runtime_;
    CallerContext caller_;
    ExecutionId execution_;
    ExecutionGeneration generation_;
    PolicyGeneration policy_generation_;
    WorkerId worker_;
    WorkerBootId boot_;
    LeaseToken token_;
    OpenOutcome outcome_;
};

}  // namespace

PEF_TEST(adversarial, torn_journal_tail_is_discarded_and_reported) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-torn");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());
    for (std::uint64_t i = 0; i < 3; ++i) {
        CompleteActionResult committed;
        PEF_REQUIRE(ctx, harness.step(i, SideEffectClass::Pure, committed).ok());
    }
    PEF_REQUIRE(ctx, harness.runtime().flush().ok());
    const std::uint64_t before = harness.journal_size();
    PEF_REQUIRE(ctx, harness.runtime().shutdown().ok());

    PEF_PHASE(ctx, PERSIST);
    // A torn tail is what a crash during an append leaves behind.
    harness.truncate_file("pef.journal", before - 6);

    PEF_PHASE(ctx, RECOVER);
    Runtime restarted;
    OpenOutcome outcome;
    PEF_REQUIRE(ctx, harness.open_fresh(restarted, false).ok());
    ExecutionView view;
    PEF_REQUIRE(ctx, restarted.query(harness.execution(), view).ok());
    PEF_CHECK_MSG(ctx, view.execution.progress_generation.established(),
                  "durable progress before the torn tail was lost");
    AuditReport audit;
    PEF_REQUIRE(ctx, restarted.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, restarted.shutdown().ok());
}

PEF_TEST(adversarial, mid_journal_corruption_is_refused) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-corrupt");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());
    for (std::uint64_t i = 0; i < 6; ++i) {
        CompleteActionResult committed;
        PEF_REQUIRE(ctx, harness.step(i, SideEffectClass::Pure, committed).ok());
    }
    PEF_REQUIRE(ctx, harness.runtime().flush().ok());
    PEF_REQUIRE(ctx, harness.runtime().shutdown().ok());

    PEF_PHASE(ctx, PERSIST);
    // Flip a byte inside the third record, leaving later records readable. This
    // is corruption, not a torn tail, and must be refused rather than silently
    // truncated.
    harness.corrupt("pef.journal", 40 + 40 + 12, 0x5A);

    PEF_PHASE(ctx, VERIFY);
    Runtime restarted;
    const Status status = harness.open_fresh(restarted, false);
    PEF_CHECK_MSG(ctx, !status.ok(), "mid-journal corruption was accepted");
    PEF_CHECK(ctx, status.code() == Code::CorruptState || status.code() == Code::TruncatedState);
}

PEF_TEST(adversarial, corrupt_snapshot_is_refused) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-snapshot");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());
    CompleteActionResult committed;
    PEF_REQUIRE(ctx, harness.step(0, SideEffectClass::Pure, committed).ok());
    PEF_REQUIRE(ctx, harness.runtime().take_snapshot(true).ok());
    PEF_REQUIRE(ctx, harness.runtime().shutdown().ok());

    PEF_PHASE(ctx, PERSIST);
    harness.corrupt("pef.snapshot", 70, 0xFF);

    PEF_PHASE(ctx, VERIFY);
    Runtime restarted;
    const Status status = harness.open_fresh(restarted, false);
    PEF_CHECK_MSG(ctx, !status.ok(), "a corrupt snapshot was accepted");
}

PEF_TEST(adversarial, truncated_snapshot_is_refused) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-snaptrunc");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.runtime().take_snapshot(true).ok());
    PEF_REQUIRE(ctx, harness.runtime().shutdown().ok());

    PEF_PHASE(ctx, PERSIST);
    harness.truncate_file("pef.snapshot", 20);

    PEF_PHASE(ctx, VERIFY);
    Runtime restarted;
    const Status status = harness.open_fresh(restarted, false);
    PEF_CHECK_MSG(ctx, !status.ok(), "a truncated snapshot was accepted");
}

PEF_TEST(adversarial, stale_authority_is_refused_in_every_domain) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-stale");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());

    PEF_PHASE(ctx, VERIFY);
    auto attempt = [&](const LeaseToken& token) {
        BeginActionRequest request;
        request.request = RequestId{5000 + token.epoch.value() + token.lease_generation.value() +
                                    token.incarnation_generation.value() +
                                    token.execution_generation.value()};
        request.token = token;
        request.effect_class = SideEffectClass::Pure;
        BeginActionResult action;
        return harness.runtime().begin_action(request, action);
    };

    // Control: the live token works.
    PEF_CHECK(ctx, attempt(harness.token()).ok());

    LeaseToken token = harness.token();
    token.epoch = CoordinatorEpoch{token.epoch.value() + 5};
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleEpoch);

    token = harness.token();
    token.execution_generation = ExecutionGeneration{token.execution_generation.value() + 1};
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleExecutionGeneration);

    token = harness.token();
    token.incarnation_generation =
        ExecutionIncarnationGeneration{token.incarnation_generation.value() + 1};
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleIncarnation);

    token = harness.token();
    token.incarnation = ExecutionIncarnationId{0xBADBAD};
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleIncarnation);

    token = harness.token();
    token.lease_generation = LeaseGeneration{token.lease_generation.value() + 1};
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleLease);

    token = harness.token();
    token.lease = LeaseId{0xFEEDFACE};
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleLease);

    token = harness.token();
    token.boot = mint_worker_boot_id(harness.worker());
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleWorkerBoot);

    token = harness.token();
    token.worker = derive_worker_id("someone-else");
    PEF_CHECK(ctx, attempt(token).code() == Code::StaleWorkerBoot);

    token = harness.token();
    token.policy_generation = PolicyGeneration{token.policy_generation.value() + 1};
    PEF_CHECK(ctx, attempt(token).code() == Code::StalePolicyGeneration);

    token = harness.token();
    token.execution = ExecutionId{0x1234567890ABCDEFULL};
    PEF_CHECK(ctx, attempt(token).code() == Code::NotFound);

    AuditReport audit;
    PEF_REQUIRE(ctx, harness.runtime().audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(adversarial, duplicate_and_reordered_completion_cannot_advance_twice) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-duplicate");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());

    PEF_PHASE(ctx, ACTION);
    BeginActionRequest begin;
    begin.request = RequestId{1000};
    begin.token = harness.token();
    begin.effect_class = SideEffectClass::Idempotent;
    BeginActionResult action;
    PEF_REQUIRE(ctx, harness.runtime().begin_action(begin, action).ok());

    CompleteActionRequest complete;
    complete.request = RequestId{1001};
    complete.token = harness.token();
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    complete.completion.effect_applied = true;
    CompleteActionResult first;
    PEF_REQUIRE(ctx, harness.runtime().complete_action(complete, first).ok());

    PEF_PHASE(ctx, COMMIT);
    // Same request identity.
    CompleteActionResult replay_same_request;
    PEF_REQUIRE(ctx, harness.runtime().complete_action(complete, replay_same_request).ok());
    PEF_CHECK(ctx, replay_same_request.commit == first.commit);
    // Different request identity, same action: still the same logical commit.
    complete.request = RequestId{1002};
    CompleteActionResult replay_new_request;
    PEF_REQUIRE(ctx, harness.runtime().complete_action(complete, replay_new_request).ok());
    PEF_CHECK(ctx, replay_new_request.duplicate);
    PEF_CHECK(ctx, replay_new_request.commit == first.commit);
    // A *new* progress generation for the same action identity must be refused.
    ExecutionView view;
    PEF_REQUIRE(ctx, harness.runtime().query(harness.execution(), view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 1);
    PEF_CHECK(ctx, view.commit_count == 1);

    PEF_PHASE(ctx, VERIFY);
    AuditReport audit;
    PEF_REQUIRE(ctx, harness.runtime().audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(adversarial, reordered_completion_for_an_unknown_action_is_refused) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-reorder");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());

    PEF_PHASE(ctx, ACTION);
    BeginActionRequest first_begin;
    first_begin.request = RequestId{1100};
    first_begin.token = harness.token();
    first_begin.effect_class = SideEffectClass::Pure;
    BeginActionResult first_action;
    PEF_REQUIRE(ctx, harness.runtime().begin_action(first_begin, first_action).ok());

    BeginActionRequest second_begin;
    second_begin.request = RequestId{1101};
    second_begin.token = harness.token();
    second_begin.effect_class = SideEffectClass::Pure;
    BeginActionResult second_action;
    PEF_REQUIRE(ctx, harness.runtime().begin_action(second_begin, second_action).ok());
    PEF_CHECK(ctx, second_action.ordinal == 2);

    PEF_PHASE(ctx, COMMIT);
    // Completing ordinal 2 before ordinal 1 leaves a hole in the durable
    // frontier. The committed progress is still exactly one transition, and the
    // auditor reports no duplicate commit.
    CompleteActionRequest complete;
    complete.request = RequestId{1102};
    complete.token = harness.token();
    complete.action = second_action.action;
    complete.action_generation = second_action.action_generation;
    complete.completion.effect_applied = true;
    CompleteActionResult out;
    PEF_REQUIRE(ctx, harness.runtime().complete_action(complete, out).ok());
    PEF_CHECK(ctx, out.ordinal == 2);

    PEF_PHASE(ctx, VERIFY);
    ExecutionView view;
    PEF_REQUIRE(ctx, harness.runtime().query(harness.execution(), view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 1);
    PEF_CHECK(ctx, view.commit_count == 1);

    // A completion for an action that was never begun must be refused.
    CompleteActionRequest unknown = complete;
    unknown.request = RequestId{1103};
    unknown.action = ActionId{0xDEADBEEF};
    CompleteActionResult unknown_out;
    const Status status = harness.runtime().complete_action(unknown, unknown_out);
    PEF_CHECK_MSG(ctx, !status.ok(), "a completion for an unknown action was accepted");
}

PEF_TEST(adversarial, checkpoint_lineage_break_is_refused) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-lineage");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());
    CompleteActionResult committed;
    PEF_REQUIRE(ctx, harness.step(0, SideEffectClass::Pure, committed).ok());

    PEF_PHASE(ctx, CHECKPOINT);
    RegisterCheckpointResult first;
    PEF_REQUIRE(ctx, harness.checkpoint(CheckpointId{}, CheckpointGeneration{}, first).ok());
    PEF_CHECK(ctx, first.lineage_depth == 0);

    // Declaring a parent that is not the current checkpoint breaks lineage.
    RegisterCheckpointRequest request;
    request.request = RequestId{9100};
    request.token = harness.token();
    request.expected_checkpoint = CheckpointId{0xABCDEF};
    request.expected_checkpoint_generation = CheckpointGeneration{9};
    request.content_hash = 0x99;
    RegisterCheckpointResult out;
    const Status status = harness.runtime().register_checkpoint(request, out);
    PEF_CHECK_MSG(ctx, !status.ok(), "a checkpoint with a broken lineage was accepted");
    PEF_CHECK(ctx, status.code() == Code::StaleCheckpointGeneration);

    // A checkpoint with no content digest cannot be validated and is refused.
    RegisterCheckpointRequest no_hash = request;
    no_hash.request = RequestId{9101};
    no_hash.expected_checkpoint = first.checkpoint;
    no_hash.expected_checkpoint_generation = first.checkpoint_generation;
    no_hash.content_hash = 0;
    const Status no_hash_status = harness.runtime().register_checkpoint(no_hash, out);
    PEF_CHECK(ctx, no_hash_status.code() == Code::InvalidArgument);

    PEF_PHASE(ctx, VERIFY);
    AuditReport audit;
    PEF_REQUIRE(ctx, harness.runtime().audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(adversarial, resume_from_a_superseded_checkpoint_is_refused) {
    PEF_PHASE(ctx, SETUP);
    Harness harness("adv-superseded");
    PEF_REQUIRE(ctx, harness.open().ok());
    PEF_REQUIRE(ctx, harness.create_execution().ok());
    PEF_REQUIRE(ctx, harness.bind().ok());
    PEF_REQUIRE(ctx, harness.start().ok());
    CompleteActionResult committed;
    PEF_REQUIRE(ctx, harness.step(0, SideEffectClass::Pure, committed).ok());

    RegisterCheckpointResult first;
    PEF_REQUIRE(ctx, harness.checkpoint(CheckpointId{}, CheckpointGeneration{}, first).ok());
    RegisterCheckpointResult second;
    PEF_REQUIRE(
        ctx, harness.checkpoint(first.checkpoint, first.checkpoint_generation, second).ok());

    PEF_PHASE(ctx, RECOVER);
    ExecutionView view;
    PEF_REQUIRE(ctx, harness.runtime().query(harness.execution(), view).ok());
    PEF_CHECK(ctx, view.checkpoints.size() == 2);
    // The earlier checkpoint is superseded and must no longer seed execution.
    const CheckpointRecord* older = nullptr;
    for (const auto& checkpoint : view.checkpoints) {
        if (checkpoint.id == first.checkpoint) {
            older = &checkpoint;
        }
    }
    PEF_REQUIRE(ctx, older != nullptr);
    PEF_CHECK(ctx, older->state == CheckpointState::Superseded);
    PEF_CHECK(ctx, !checkpoint_can_seed_continuation(older->state));

    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    PEF_REQUIRE(ctx,
                harness.runtime()
                    .validate_continuation(harness.caller(), harness.execution(), state, reason)
                    .ok());
    PEF_CHECK(ctx, state == ContinuationState::Valid);

    AuditReport audit;
    PEF_REQUIRE(ctx, harness.runtime().audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(adversarial, malformed_frames_are_refused) {
    PEF_PHASE(ctx, CREATE);
    Frame frame;
    frame.type = MessageType::Query;
    frame.request = RequestId{7};
    frame.payload = encode_payload(PingRequest{42});
    const Bytes good = encode_frame(frame);

    PEF_PHASE(ctx, VERIFY);
    Frame parsed;
    std::string reason;
    PEF_CHECK(ctx, decode_frame(good, parsed, reason));
    PEF_CHECK(ctx, parsed.request == frame.request);

    // Wrong magic.
    Bytes bad_magic = good;
    bad_magic[0] = std::byte{0x00};
    PEF_CHECK(ctx, !decode_frame(bad_magic, parsed, reason));

    // Unsupported version.
    Bytes bad_version = good;
    bad_version[4] = std::byte{0x7F};
    PEF_CHECK(ctx, !decode_frame(bad_version, parsed, reason));

    // Unknown message type.
    Bytes bad_type = good;
    bad_type[6] = std::byte{0xEE};
    bad_type[7] = std::byte{0xEE};
    PEF_CHECK(ctx, !decode_frame(bad_type, parsed, reason));

    // Non-zero reserved flags.
    Bytes bad_flags = good;
    bad_flags[20] = std::byte{0x01};
    PEF_CHECK(ctx, !decode_frame(bad_flags, parsed, reason));

    // Truncated frame.
    Bytes truncated(good.begin(), good.end() - 3);
    PEF_CHECK(ctx, !decode_frame(truncated, parsed, reason));

    // Extended frame.
    Bytes extended = good;
    extended.push_back(std::byte{0x00});
    PEF_CHECK(ctx, !decode_frame(extended, parsed, reason));

    // Corrupted payload byte.
    Bytes bad_payload = good;
    bad_payload[kFrameHeaderSize] = std::byte{0xFF};
    PEF_CHECK(ctx, !decode_frame(bad_payload, parsed, reason));

    // A declared payload length far beyond the protocol bound. The header is
    // rebuilt so that the header checksum is valid and only the bound can
    // reject it.
    Bytes oversized = good;
    oversized[16] = std::byte{0xFF};
    oversized[17] = std::byte{0xFF};
    oversized[18] = std::byte{0xFF};
    oversized[19] = std::byte{0x7F};
    const std::uint64_t header_crc = crc64_ecma(oversized.data(), 24);
    for (int i = 0; i < 4; ++i) {
        oversized[24 + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((header_crc >> (8 * i)) & 0xFFu);
    }
    PEF_CHECK(ctx, !decode_frame(oversized, parsed, reason));

    // A reply frame is not a valid request.
    Frame reply;
    reply.type = static_cast<MessageType>(static_cast<std::uint16_t>(MessageType::Query) |
                                          kReplyBit);
    PEF_CHECK(ctx, decode_frame(encode_frame(reply), parsed, reason));
    PEF_CHECK(ctx, parsed.is_reply());
}

PEF_TEST(adversarial, hostile_persisted_payloads_are_refused) {
    PEF_PHASE(ctx, CREATE);
    // A blob whose declared length exceeds the decoder bound.
    ByteWriter writer;
    writer.u64(0xFFFFFFFFFFFFULL);
    ByteReader reader(writer.bytes());
    const Bytes blob = reader.blob();
    PEF_CHECK(ctx, blob.empty());
    PEF_CHECK(ctx, reader.failed());

    PEF_PHASE(ctx, VERIFY);
    // A durable record whose enum ordinal is outside the valid range. The
    // lifecycle byte follows four 64-bit identity fields.
    ExecutionRecord record;
    record.id = ExecutionId{1};
    record.generation = ExecutionGeneration{1};
    record.lifecycle = Lifecycle::Retired;
    const Bytes encoded = encode_to_bytes(record);
    PEF_REQUIRE(ctx, encoded.size() > 32);
    PEF_CHECK(ctx, encoded[32] == std::byte{14});
    Bytes hostile = encoded;
    hostile[32] = std::byte{0x7F};
    ExecutionRecord decoded_record;
    PEF_CHECK(ctx, !decode_from_bytes(hostile, decoded_record));

    // A journal record whose payload does not decode is refused on load.
    DurableState state;
    Bytes garbage(16, std::byte{0xAB});
    PEF_CHECK(ctx, !apply_record(state, RecordKind::Execution, garbage));
    PEF_CHECK(ctx, !apply_record(state, RecordKind::StoreMeta, garbage));
    PEF_CHECK(ctx, !apply_record(state, RecordKind::Barrier, garbage));
}
