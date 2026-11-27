#ifndef BOSHIAMY_WINDOWS_DOCTOR_RUNTIME_HPP
#define BOSHIAMY_WINDOWS_DOCTOR_RUNTIME_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "doctor_store.hpp"
#include "tsf/doctor_transport.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace boshiamy::doctor {

class DoctorRuntimeHolder;

struct DoctorRuntimeOpenResult {
    std::unique_ptr<DoctorRuntimeHolder> holder;
    DoctorStoreFailure failure = DoctorStoreFailure::None;
    std::int32_t platform_result_code = 0;
    bool recovered_abandoned = false;

    bool ok() const noexcept {
        return holder != nullptr && failure == DoctorStoreFailure::None;
    }
};

namespace doctor_runtime_detail {

inline constexpr std::uint32_t kFocusFlag = 1U << 0;
inline constexpr std::uint32_t kEngineFlag = 1U << 1;
inline constexpr std::uint32_t kEnglishFlag = 1U << 2;
inline constexpr std::uint32_t kComposingFlag = 1U << 3;
inline constexpr std::uint32_t kHandledFlag = 1U << 4;
inline constexpr std::uint32_t kKnownFlags =
    kFocusFlag | kEngineFlag | kEnglishFlag | kComposingFlag | kHandledFlag;
inline constexpr std::uint64_t kSigned63Max =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

inline DoctorStoreActionResult action_result(
    DoctorStoreFailure failure,
    std::int32_t platform_result_code = 0) {
    return DoctorStoreActionResult{failure, platform_result_code};
}

inline DoctorStoreFailure failure_from_hresult(HRESULT result) noexcept {
    const auto raw = static_cast<LONG>(result);
    if (result == E_ACCESSDENIED ||
        raw == static_cast<LONG>(
            HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) ||
        raw == static_cast<LONG>(
            HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) ||
        raw == static_cast<LONG>(
            HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)) ||
        raw == static_cast<LONG>(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))) {
        return DoctorStoreFailure::PermissionDenied;
    }
    return DoctorStoreFailure::IoError;
}

inline DoctorStoreActionResult action_result(HRESULT result) {
    return action_result(
        failure_from_hresult(result), static_cast<std::int32_t>(result));
}

inline bool last_dacl_matches(
    HANDLE object,
    PSECURITY_DESCRIPTOR expected) {
    if (!object || !expected) return false;

    DWORD needed = 0;
    const SECURITY_INFORMATION requested =
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
    if (!GetKernelObjectSecurity(object, requested, nullptr, 0, &needed)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0)
            return false;
    }
    std::vector<unsigned char> bytes(needed);
    if (!GetKernelObjectSecurity(
            object, requested, bytes.data(), needed, &needed))
        return false;

    PSECURITY_DESCRIPTOR actual =
        reinterpret_cast<PSECURITY_DESCRIPTOR>(bytes.data());
    PSID expected_owner = nullptr;
    PSID actual_owner = nullptr;
    BOOL owner_defaulted = FALSE;
    HANDLE token = nullptr;
    std::vector<unsigned char> own_sid;
    const bool token_read =
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    const HRESULT owner_result = token_read
        ? detail::token_sid_copy(token, &own_sid)
        : detail::hresult_from_last_error();
    if (token) CloseHandle(token);
    if (FAILED(owner_result) ||
        !GetSecurityDescriptorOwner(
            actual, &actual_owner, &owner_defaulted) ||
        !actual_owner ||
        !GetSecurityDescriptorOwner(
            expected, &expected_owner, &owner_defaulted) ||
        (expected_owner &&
            !EqualSid(actual_owner, expected_owner)) ||
        !EqualSid(actual_owner, own_sid.data()))
        return false;

    PACL expected_dacl = nullptr;
    PACL actual_dacl = nullptr;
    BOOL expected_present = FALSE;
    BOOL actual_present = FALSE;
    BOOL defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(
            expected, &expected_present, &expected_dacl, &defaulted) ||
        !GetSecurityDescriptorDacl(
            actual, &actual_present, &actual_dacl, &defaulted) ||
        !expected_present || !actual_present ||
        !expected_dacl || !actual_dacl ||
        expected_dacl->AclSize != actual_dacl->AclSize ||
        std::memcmp(
            expected_dacl,
            actual_dacl,
            expected_dacl->AclSize) != 0)
        return false;

    SECURITY_DESCRIPTOR_CONTROL expected_control = 0;
    SECURITY_DESCRIPTOR_CONTROL actual_control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(
            expected, &expected_control, &revision) ||
        !GetSecurityDescriptorControl(
            actual, &actual_control, &revision))
        return false;
    if ((expected_control & SE_DACL_PROTECTED) != (actual_control & SE_DACL_PROTECTED))
        return false;
    PACL labels = nullptr;
    BOOL labels_present = FALSE;
    if (!GetSecurityDescriptorSacl(actual, &labels_present, &labels, &defaulted)) return false;
    // Windows treats an unlabeled object as medium integrity. Explicit low
    // labels must be rejected even when the owner and protected DACL match.
    bool seen_label = false;
    if (labels_present && labels) for (DWORD index = 0; index < labels->AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(labels, index, &raw)) return false;
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
        if (seen_label || header->AceSize < sizeof(SYSTEM_MANDATORY_LABEL_ACE)) return false;
        seen_label = true;
        const auto* label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(raw);
        PSID sid = const_cast<DWORD*>(&label->SidStart);
        const SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        if (!IsValidSid(sid) || *GetSidSubAuthorityCount(sid) != 1 ||
            std::memcmp(GetSidIdentifierAuthority(sid), &authority, sizeof(authority)) != 0 ||
            *GetSidSubAuthority(sid, 0) < SECURITY_MANDATORY_MEDIUM_RID ||
            !(label->Mask & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP)) return false;
    }
    return true;
}

inline std::int64_t positive_id(std::uint64_t value) noexcept {
    const auto positive = value & kSigned63Max;
    return positive == 0 ? 0 : static_cast<std::int64_t>(positive);
}

inline bool signed_id(std::uint64_t value, std::int64_t *output) noexcept {
    if (!output || value > kSigned63Max) return false;
    *output = static_cast<std::int64_t>(value);
    return true;
}

inline bool lifecycle_event(Event event) noexcept {
    switch (event) {
    case Event::Created:
    case Event::ActivateBegin:
    case Event::ActivateEnd:
    case Event::Focus:
    case Event::Deactivate:
        return true;
    default:
        return false;
    }
}

inline DoctorCallbackSource callback_source(const EventRecord &record) noexcept {
    const auto event = static_cast<Event>(record.event_code);
    if (lifecycle_event(event)) return DoctorCallbackSource::None;
    if (record.callback_source >=
            static_cast<std::uint32_t>(DoctorCallbackSource::Dispatch) &&
        record.callback_source <=
            static_cast<std::uint32_t>(DoctorCallbackSource::KeyAction))
        return static_cast<DoctorCallbackSource>(record.callback_source);
    switch (event) {
    case Event::KeySinkAdvice:
    case Event::ExtendedInterfaceQuery:
        return DoctorCallbackSource::Dispatch;
    case Event::TestDown:
    case Event::KeyDown:
    case Event::ShiftTestUp:
    case Event::ShiftUp:
        return DoctorCallbackSource::KeyAction;
    case Event::EditRequest:
    case Event::EditApplied:
        return DoctorCallbackSource::SlotForward;
    default:
        return DoctorCallbackSource::None;
    }
}

inline DoctorEventPhase phase(const EventRecord &record) noexcept {
    if (lifecycle_event(static_cast<Event>(record.event_code)))
        return DoctorEventPhase::Lifecycle;
    switch (static_cast<Event>(record.event_code)) {
    case Event::KeySinkAdvice:
        return DoctorEventPhase::Lifecycle;
    case Event::TestDown:
    case Event::KeyDown:
    case Event::EditRequest:
        return DoctorEventPhase::Composition;
    case Event::EditApplied:
        return (record.flags & kComposingFlag) != 0
            ? DoctorEventPhase::Composition
            : DoctorEventPhase::Commit;
    case Event::ExtendedInterfaceQuery:
        return DoctorEventPhase::Candidate;
    case Event::ShiftTestUp:
    case Event::ShiftUp:
        return (record.flags & kComposingFlag) != 0
            ? DoctorEventPhase::Composition
            : DoctorEventPhase::Candidate;
    default:
        return DoctorEventPhase::None;
    }
}

inline DoctorResultCode fixed_result(std::int32_t platform_result) noexcept {
    const HRESULT result = static_cast<HRESULT>(platform_result);
    if (SUCCEEDED(result)) return DoctorResultCode::Ok;
    if (result == E_ABORT ||
        result == E_ACCESSDENIED ||
        result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return DoctorResultCode::Rejected;
    if (result == E_PENDING ||
        result == E_NOTIMPL ||
        result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))
        return DoctorResultCode::Unavailable;
    return DoctorResultCode::Failed;
}

inline std::int32_t timezone_offset_minutes() noexcept {
    TIME_ZONE_INFORMATION zone{};
    const DWORD state = GetTimeZoneInformation(&zone);
    LONG bias = zone.Bias;
    if (state == TIME_ZONE_ID_STANDARD)
        bias += zone.StandardBias;
    else if (state == TIME_ZONE_ID_DAYLIGHT)
        bias += zone.DaylightBias;
    const LONG offset = -bias;
    if (offset < -12 * 60) return -12 * 60;
    if (offset > 14 * 60) return 14 * 60;
    return static_cast<std::int32_t>(offset);
}

} // namespace doctor_runtime_detail

class DoctorRuntimeOwnership {
public:
    DoctorRuntimeOwnership() = default;
    DoctorRuntimeOwnership(const DoctorRuntimeOwnership &) = delete;
    DoctorRuntimeOwnership &operator=(
        const DoctorRuntimeOwnership &) = delete;

    DoctorRuntimeOwnership(DoctorRuntimeOwnership &&other) noexcept
        : mutex_(other.mutex_),
          owned_(other.owned_),
          recovered_abandoned_(other.recovered_abandoned_),
          owner_thread_id_(other.owner_thread_id_),
          result_(other.result_) {
        other.mutex_ = nullptr;
        other.owned_ = false;
        other.recovered_abandoned_ = false;
        other.owner_thread_id_ = 0;
        other.result_ = static_cast<HRESULT>(E_UNEXPECTED);
    }

    ~DoctorRuntimeOwnership() {
        if (owned_ && mutex_ &&
            owner_thread_id_ == GetCurrentThreadId())
            ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }

    static std::wstring mutex_name(const std::wstring &user_sid) {
        return user_sid.empty()
            ? std::wstring()
            : L"Global\\LiuKaiDoctorWriter-" + user_sid;
    }

    bool try_acquire() {
        auto security = UserSecurity::create_mutex();
        if (FAILED(security.result)) {
            result_ = security.result;
            return false;
        }

        std::wstring user_sid;
        if (!detail::own_user_sid(&user_sid)) {
            result_ = static_cast<HRESULT>(E_ACCESSDENIED);
            return false;
        }
        const auto name = mutex_name(user_sid);
        if (name.empty()) {
            result_ = static_cast<HRESULT>(E_OUTOFMEMORY);
            return false;
        }

        mutex_ = CreateMutexW(
            &security.attributes, FALSE, name.c_str());
        if (!mutex_) {
            result_ = detail::hresult_from_last_error();
            return false;
        }
        if (!doctor_runtime_detail::last_dacl_matches(
                mutex_, security.descriptor)) {
            result_ = static_cast<HRESULT>(E_ACCESSDENIED);
            CloseHandle(mutex_);
            mutex_ = nullptr;
            return false;
        }

        const DWORD wait = WaitForSingleObject(mutex_, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            owned_ = true;
            recovered_abandoned_ = wait == WAIT_ABANDONED;
            owner_thread_id_ = GetCurrentThreadId();
            result_ = S_OK;
            return true;
        }
        if (wait == WAIT_TIMEOUT) {
            result_ = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        } else {
            result_ = detail::hresult_from_last_error();
        }
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }

    bool recovered_abandoned() const noexcept {
        return recovered_abandoned_;
    }

    HRESULT result() const noexcept { return result_; }

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
    bool recovered_abandoned_ = false;
    DWORD owner_thread_id_ = 0;
    HRESULT result_ = static_cast<HRESULT>(E_UNEXPECTED);
};

class DoctorRuntimeHolder {
public:
    static constexpr std::size_t kMaximumAttachments = 64;
    DoctorRuntimeHolder(const DoctorRuntimeHolder &) = delete;
    DoctorRuntimeHolder &operator=(
        const DoctorRuntimeHolder &) = delete;
    DoctorRuntimeHolder(DoctorRuntimeHolder &&) = delete;
    DoctorRuntimeHolder &operator=(DoctorRuntimeHolder &&) = delete;

    ~DoctorRuntimeHolder() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        stop_queues_locked();
    }

    static DoctorRuntimeOpenResult open(
        const std::filesystem::path &root,
        const DoctorBuildMetadata &build,
        const DoctorClock &clock,
        DoctorStoreLimits limits = DoctorStoreLimits::defaults(),
        DoctorIoFaults *io_faults = nullptr,
        std::size_t maximum_attachments = kMaximumAttachments) {
        if (maximum_attachments == 0 || maximum_attachments > kMaximumAttachments)
            return {nullptr, DoctorStoreFailure::InvalidSchema,
                static_cast<std::int32_t>(E_INVALIDARG), false};
        DoctorRuntimeOwnership ownership;
        try {
            if (!ownership.try_acquire()) {
                return DoctorRuntimeOpenResult{
                    nullptr,
                    doctor_runtime_detail::failure_from_hresult(
                        ownership.result()),
                    static_cast<std::int32_t>(ownership.result()),
                    false,
                };
            }
            auto store = DoctorStore::open(
                root, build, clock, limits, io_faults);
            if (!store) {
                return DoctorRuntimeOpenResult{
                    nullptr,
                    DoctorStoreFailure::IoError,
                    static_cast<std::int32_t>(E_UNEXPECTED),
                    ownership.recovered_abandoned(),
                };
            }
            const bool recovered = ownership.recovered_abandoned();
            return DoctorRuntimeOpenResult{
                std::unique_ptr<DoctorRuntimeHolder>(
                    new DoctorRuntimeHolder(
                        std::move(ownership), std::move(store), clock, maximum_attachments)),
                DoctorStoreFailure::None,
                0,
                recovered,
            };
        } catch (const std::bad_alloc &) {
            return DoctorRuntimeOpenResult{
                nullptr,
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_OUTOFMEMORY),
                ownership.recovered_abandoned(),
            };
        }
    }

    DoctorStoreState status() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        return status_locked();
    }

    DoctorStoreActionResult enable() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto persisted = store_->enable();
        if (!persisted.ok()) {
            fail_closed_locked(persisted);
            return persisted;
        }
        runtime_failure_ = DoctorStoreFailure::None;
        runtime_platform_result_ = 0;
        last_health_ns_ = clock_.monotonic_ns();
        return activate_queues_locked();
    }

    DoctorStoreActionResult disable() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        auto stopped = stop_queues_locked();
        const auto persisted = store_->disable();
        if (!persisted.ok()) {
            remember_failure_locked(persisted);
            return persisted;
        }
        if (!stopped.ok()) {
            remember_failure_locked(stopped);
            return stopped;
        }
        runtime_failure_ = DoctorStoreFailure::None;
        runtime_platform_result_ = 0;
        return {};
    }

    DoctorStoreResult<DoctorExportSnapshot> prepare(
        std::int64_t selected_time_ms,
        std::int64_t before_ms,
        std::int64_t after_ms) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto drained = drain_locked();
        if (!drained.ok()) {
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, drained.failure, drained.platform_result_code};
        }
        const auto mark = store_->mark_issue(
            selected_time_ms, DoctorExportWindow{before_ms, after_ms});
        if (!mark.ok()) {
            observe_store_failure_locked();
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, mark.failure, mark.platform_result_code};
        }
        const auto snapshot = store_->prepare_export(mark.value);
        if (!snapshot.ok()) observe_store_failure_locked();
        return snapshot;
    }

    DoctorStoreResult<DoctorIssueMark> mark(
        std::int64_t selected_time_ms,
        std::int64_t before_ms,
        std::int64_t after_ms) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto mark = store_->mark_issue(
            selected_time_ms, DoctorExportWindow{before_ms, after_ms});
        if (!mark.ok()) observe_store_failure_locked();
        return mark;
    }

    DoctorStoreResult<DoctorExportSnapshot> prepare(
        const DoctorIssueMark &mark) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto drained = drain_locked();
        if (!drained.ok()) {
            return DoctorStoreResult<DoctorExportSnapshot>{
                {}, drained.failure, drained.platform_result_code};
        }
        const auto snapshot = store_->prepare_export(mark);
        if (!snapshot.ok()) observe_store_failure_locked();
        return snapshot;
    }

    DoctorStoreResult<DoctorExportPreview> preview(
        const DoctorExportSnapshot &snapshot) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto result = store_->preview_export(snapshot);
        if (!result.ok()) observe_store_failure_locked();
        return result;
    }

    DoctorStoreActionResult save(
        const DoctorExportSnapshot &snapshot,
        const std::filesystem::path &destination) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (snapshot.epoch != store_->status().epoch)
            return doctor_runtime_detail::action_result(
                DoctorStoreFailure::EpochMismatch);
        const auto result = store_->save_export(snapshot, destination);
        if (!result.ok()) observe_store_failure_locked();
        return result;
    }

    DoctorStoreActionResult cancel(
        const DoctorExportSnapshot &snapshot) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto result = store_->cancel_export(snapshot);
        if (!result.ok()) observe_store_failure_locked();
        return result;
    }

    DoctorStoreActionResult recover_pending_export() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto result = store_->cancel_active_export();
        if (!result.ok()) observe_store_failure_locked();
        return result;
    }

    DoctorStoreActionResult clear() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto stopped = stop_queues_locked();
        if (!stopped.ok()) {
            remember_failure_locked(stopped);
            return stopped;
        }
        const auto cleared = store_->clear();
        if (!cleared.ok()) {
            remember_failure_locked(cleared);
            return cleared;
        }
        runtime_failure_ = DoctorStoreFailure::None;
        runtime_platform_result_ = 0;
        return activate_queues_locked();
    }

    DoctorStoreActionResult attach(
        DWORD process_id,
        std::uint64_t process_creation) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (process_id == 0 || process_creation == 0)
            return doctor_runtime_detail::action_result(
                DoctorStoreFailure::InvalidSchema,
                static_cast<std::int32_t>(E_INVALIDARG));

        reap_finished_attachments_locked();
        for (const auto &attachment : attachments_)
            if (attachment.process_id == process_id &&
                attachment.process_creation == process_creation) return {};

        DWORD session_id = 0;
        if (!ProcessIdToSessionId(process_id, &session_id))
            return doctor_runtime_detail::action_result(
                detail::hresult_from_last_error());
        if (session_id == 0)
            return doctor_runtime_detail::action_result(
                DoctorStoreFailure::InvalidSchema,
                static_cast<std::int32_t>(E_INVALIDARG));

        std::unique_ptr<void, CloseProcessHandle> process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process_id));
        if (!process) return doctor_runtime_detail::action_result(detail::hresult_from_last_error());
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
            return doctor_runtime_detail::action_result(detail::hresult_from_last_error());
        ULARGE_INTEGER stamp{};
        stamp.LowPart = created.dwLowDateTime;
        stamp.HighPart = created.dwHighDateTime;
        if (stamp.QuadPart != process_creation ||
            WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
            return doctor_runtime_detail::action_result(DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_INVALID_STATE)));

        auto view = std::make_unique<ProcessCollectorView>(
            process_id, process_creation);
        if (!view->open())
            return doctor_runtime_detail::action_result(view->last_hr());
        if (attachments_.size() >= maximum_attachments_) {
            producer_limit_reached_ = true;
            return doctor_runtime_detail::action_result(DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES)));
        }
        const auto state = store_->status();
        const bool enabled = state.enabled &&
            !state.pending_clear &&
            state.failure == DoctorStoreFailure::None &&
            runtime_failure_ == DoctorStoreFailure::None;
        if (!view->initialize(
                static_cast<std::uint64_t>(state.epoch), enabled))
            return doctor_runtime_detail::action_result(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_UNEXPECTED));

        attachments_.push_back(Attachment{
            process_id, process_creation, session_id, std::move(view), std::move(process)});
        if (!enabled) {
            auto &attachment = attachments_.back();
            return discard_attachment_locked(&attachment);
        }
        return {};
    }

    DoctorStoreActionResult drain() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto result = drain_locked();
        reap_finished_attachments_locked(true);
        return result;
    }

    std::size_t attachment_count() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        return attachments_.size();
    }

    DoctorStoreActionResult heartbeat() {
        std::lock_guard<std::mutex> lock(command_mutex_);
        const auto state = status_locked();
        const auto now = clock_.monotonic_ns();
        if (!state.collecting || now < last_health_ns_ ||
            now - last_health_ns_ < 10'000'000'000LL ||
            health_sequence_ == std::numeric_limits<std::int64_t>::max()) return {};
        last_health_ns_ = now;
        DoctorEventRecord event{};
        event.producer_id = health_producer_id_;
        event.session_id = health_session_id_;
        event.epoch = state.epoch;
        event.sequence = ++health_sequence_;
        event.monotonic_ns = now;
        event.wall_utc_ms = clock_.wall_utc_ms();
        event.timezone_offset_minutes = doctor_runtime_detail::timezone_offset_minutes();
        event.event_code = DoctorEventCode::Health;
        event.callback_source = DoctorCallbackSource::None;
        event.scope = DoctorInputScope::Unknown;
        event.mode = DoctorEventMode::None;
        event.phase = DoctorEventPhase::Health;
        event.handled = true;
        const auto appended = store_->append(event);
        if (!appended.ok()) fail_closed_locked(appended);
        return appended;
    }

private:
    struct CloseProcessHandle {
        void operator()(void *handle) const noexcept { if (handle) CloseHandle(handle); }
    };
    struct Attachment {
        DWORD process_id = 0;
        std::uint64_t process_creation = 0;
        DWORD session_id = 0;
        std::unique_ptr<ProcessCollectorView> view;
        std::unique_ptr<void, CloseProcessHandle> process;
    };

    void reap_finished_attachments_locked(bool drained = false) {
        attachments_.erase(std::remove_if(attachments_.begin(), attachments_.end(),
            [drained](const Attachment &attachment) {
                return attachment.process &&
                    WaitForSingleObject(attachment.process.get(), 0) == WAIT_OBJECT_0 &&
                    attachment.view && (drained || attachment.view->snapshot().queued == 0);
            }), attachments_.end());
        if (attachments_.size() < maximum_attachments_) producer_limit_reached_ = false;
    }

    DoctorRuntimeHolder(
        DoctorRuntimeOwnership ownership,
        std::unique_ptr<DoctorStore> store,
        const DoctorClock &clock,
        std::size_t maximum_attachments)
        : ownership_(std::move(ownership)),
          store_(std::move(store)),
          clock_(clock), maximum_attachments_(maximum_attachments),
          last_health_ns_(clock.monotonic_ns()),
          health_producer_id_(static_cast<std::int64_t>(detail::random_producer_id(
              static_cast<std::uint64_t>(clock.monotonic_ns()), GetCurrentProcessId()))) {
        DWORD session = 0;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session) && session)
            health_session_id_ = session;
    }

    DoctorStoreState status_locked() const {
        auto result = store_->status();
        result.producer_limit_reached = producer_limit_reached_;
        if (runtime_failure_ != DoctorStoreFailure::None) {
            result.failure = runtime_failure_;
            result.platform_result_code = runtime_platform_result_;
            result.collecting = false;
            if (runtime_failure_ == DoctorStoreFailure::EpochCommitFailed) {
                result.has_retained_range = false;
                result.oldest_retained_ms = result.newest_retained_ms = 0;
            }
        }
        return result;
    }

    void remember_failure_locked(
        const DoctorStoreActionResult &failure) noexcept {
        if (!failure.ok()) {
            runtime_failure_ = failure.failure;
            runtime_platform_result_ = failure.platform_result_code;
        }
    }

    void fail_closed_locked(
        const DoctorStoreActionResult &failure) {
        remember_failure_locked(failure);
        stop_queues_locked();
    }

    void observe_store_failure_locked() {
        const auto state = store_->status();
        if (state.failure != DoctorStoreFailure::None ||
            state.pending_clear)
            stop_queues_locked();
    }

    DoctorStoreActionResult activate_queues_locked() {
        const auto state = status_locked();
        const bool enabled = state.collecting;
        for (auto &attachment : attachments_) {
            if (!attachment.view ||
                !attachment.view->initialize(
                    static_cast<std::uint64_t>(state.epoch), enabled)) {
                const auto failure =
                    doctor_runtime_detail::action_result(
                        DoctorStoreFailure::IoError,
                        static_cast<std::int32_t>(E_UNEXPECTED));
                fail_closed_locked(failure);
                return failure;
            }
        }
        return {};
    }

    DoctorStoreActionResult discard_attachment_locked(
        Attachment *attachment) const {
        if (!attachment || !attachment->view)
            return doctor_runtime_detail::action_result(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_UNEXPECTED));
        for (std::size_t count = 0;
                count < ProcessCollectorView::Queue::capacity() * 2;
                ++count) {
            EventRecord record{};
            const auto result = attachment->view->read(&record);
            if (result == ReadResult::Empty) return {};
            if (result != ReadResult::Read)
                return doctor_runtime_detail::action_result(
                    DoctorStoreFailure::IoError,
                    static_cast<std::int32_t>(E_UNEXPECTED));
        }
        return doctor_runtime_detail::action_result(
            DoctorStoreFailure::IoError,
            static_cast<std::int32_t>(E_UNEXPECTED));
    }

    DoctorStoreActionResult stop_queues_locked() {
        bool gated = true;
        for (auto &attachment : attachments_) {
            if (!attachment.view || !attachment.view->disable())
                gated = false;
        }
        DoctorStoreActionResult discarded{};
        for (auto &attachment : attachments_) {
            const auto result =
                discard_attachment_locked(&attachment);
            if (!result.ok() && discarded.ok()) discarded = result;
        }
        if (!gated) {
            return doctor_runtime_detail::action_result(
                DoctorStoreFailure::IoError,
                static_cast<std::int32_t>(E_UNEXPECTED));
        }
        return discarded;
    }

    DoctorStoreActionResult drain_locked() {
        auto state = status_locked();
        if (!state.collecting) {
            const auto discarded = stop_queues_locked();
            if (!discarded.ok()) return discarded;
            if (state.enabled && !state.pending_clear &&
                state.failure != DoctorStoreFailure::None) {
                return doctor_runtime_detail::action_result(
                    state.failure, state.platform_result_code);
            }
            return {};
        }

        for (auto &attachment : attachments_) {
            if (!attachment.view ||
                !attachment.view->refresh_heartbeat()) {
                const auto failure =
                    doctor_runtime_detail::action_result(
                        DoctorStoreFailure::IoError,
                        static_cast<std::int32_t>(E_UNEXPECTED));
                fail_closed_locked(failure);
                return failure;
            }
            for (std::size_t count = 0;
                    count < ProcessCollectorView::Queue::capacity() * 2;
                    ++count) {
                EventRecord queued{};
                std::uint64_t queued_epoch = 0;
                const auto read = attachment.view->read(
                    &queued, &queued_epoch);
                if (read == ReadResult::Empty) break;
                if (read != ReadResult::Read) {
                    const auto failure =
                        doctor_runtime_detail::action_result(
                            DoctorStoreFailure::IoError,
                            static_cast<std::int32_t>(E_UNEXPECTED));
                    fail_closed_locked(failure);
                    return failure;
                }
                if (queued_epoch !=
                    static_cast<std::uint64_t>(state.epoch))
                    continue;

                DoctorEventRecord event{};
                if (!convert_record_locked(
                        attachment, queued, &event))
                    continue;
                const auto appended = store_->append(event);
                if (!appended.ok()) {
                    fail_closed_locked(appended);
                    return appended;
                }
                state = status_locked();
                if (!state.collecting) break;
            }
            if (!state.collecting) break;
        }
        return {};
    }

    bool convert_record_locked(
        const Attachment &attachment,
        const EventRecord &queued,
        DoctorEventRecord *output) const {
        if (!output || queued.schema_version != kSchemaVersion ||
            queued.reserved != 0 ||
            (queued.flags & ~doctor_runtime_detail::kKnownFlags) != 0 ||
            queued.event_code < static_cast<std::uint32_t>(Event::Created) ||
            queued.event_code >
                static_cast<std::uint32_t>(Event::ShiftUp))
            return false;
        const auto event = static_cast<Event>(queued.event_code);
        if (queued.scope != static_cast<std::uint32_t>(ScopeClass::Unknown) &&
            queued.scope != static_cast<std::uint32_t>(ScopeClass::Ordinary) &&
            queued.scope != static_cast<std::uint32_t>(ScopeClass::Sensitive))
            return false;
        if (queued.scope !=
                static_cast<std::uint32_t>(ScopeClass::Ordinary) &&
            !doctor_runtime_detail::lifecycle_event(event))
            return false;

        const std::int64_t producer_id =
            doctor_runtime_detail::positive_id(queued.producer_id);
        std::int64_t operation_id = 0;
        std::int64_t sequence = 0;
        std::int64_t monotonic_ns = 0;
        const bool lifecycle =
            doctor_runtime_detail::lifecycle_event(event);
        if (producer_id <= 0 ||
            (!lifecycle && !doctor_runtime_detail::signed_id(
                queued.operation_id, &operation_id)) ||
            !doctor_runtime_detail::signed_id(
                queued.sequence, &sequence) ||
            !doctor_runtime_detail::signed_id(
                queued.monotonic_ns, &monotonic_ns))
            return false;
        const auto state = store_->status();
        const std::int64_t wall_ms = clock_.wall_utc_ms();
        if (wall_ms < 0 ||
            state.epoch < 0 ||
            attachment.session_id == 0)
            return false;

        DoctorEventRecord converted{};
        converted.producer_id = producer_id;
        converted.session_id =
            static_cast<std::int64_t>(attachment.session_id);
        converted.epoch = state.epoch;
        converted.operation_id = operation_id;
        converted.sequence = sequence;
        converted.monotonic_ns = monotonic_ns;
        converted.wall_utc_ms = wall_ms;
        converted.timezone_offset_minutes =
            doctor_runtime_detail::timezone_offset_minutes();
        converted.event_code =
            static_cast<DoctorEventCode>(queued.event_code);
        converted.callback_source =
            doctor_runtime_detail::callback_source(queued);
        converted.scope = static_cast<DoctorInputScope>(queued.scope);
        converted.mode =
            (queued.flags & doctor_runtime_detail::kEngineFlag) == 0
                ? DoctorEventMode::None
                : ((queued.flags & doctor_runtime_detail::kEnglishFlag) != 0
                    ? DoctorEventMode::English
                    : DoctorEventMode::Liu);
        converted.phase = doctor_runtime_detail::phase(queued);
        converted.focus =
            (queued.flags & doctor_runtime_detail::kFocusFlag) != 0;
        converted.handled =
            (queued.flags & doctor_runtime_detail::kHandledFlag) != 0;
        converted.result_code =
            doctor_runtime_detail::fixed_result(queued.result_code);
        converted.duration_us =
            static_cast<std::int64_t>(queued.duration_us);
        if (queued.result_code != S_OK)
            converted.platform_result_code = queued.result_code;
        if (converted.phase == DoctorEventPhase::None)
            return false;
        *output = converted;
        return true;
    }

    DoctorRuntimeOwnership ownership_;
    std::unique_ptr<DoctorStore> store_;
    std::vector<Attachment> attachments_;
    std::size_t maximum_attachments_;
    bool producer_limit_reached_ = false;
    std::int64_t last_health_ns_;
    std::int64_t health_producer_id_;
    std::int64_t health_session_id_ = 1;
    std::int64_t health_sequence_ = 0;
    const DoctorClock &clock_;
    std::mutex command_mutex_;
    DoctorStoreFailure runtime_failure_ = DoctorStoreFailure::None;
    std::int32_t runtime_platform_result_ = 0;
};

} // namespace boshiamy::doctor

#endif
