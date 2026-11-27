#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../tsf/doctor_transport.hpp"
#include "../tsf/doctor_scope.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

using namespace boshiamy::doctor;

static_assert(std::is_trivially_copyable_v<EventRecord>);
static_assert(std::is_standard_layout_v<EventRecord>);
static_assert(sizeof(EventRecord) == 64);
static_assert(std::is_same_v<decltype(EventRecord::producer_id), std::uint64_t>);
static_assert(std::is_same_v<decltype(EventRecord::operation_id), std::uint64_t>);
static_assert(std::is_same_v<decltype(EventRecord::sequence), std::uint64_t>);
static_assert(std::is_same_v<decltype(EventRecord::monotonic_ns), std::uint64_t>);
static_assert(std::is_same_v<decltype(EventRecord::schema_version), std::uint32_t>);
static_assert(std::is_same_v<decltype(EventRecord::event_code), std::uint32_t>);
static_assert(std::is_same_v<decltype(EventRecord::callback_source), std::uint32_t>);
static_assert(std::is_same_v<decltype(EventRecord::scope), std::uint32_t>);
static_assert(std::is_same_v<decltype(EventRecord::result_code), std::int32_t>);
static_assert(std::is_same_v<decltype(EventRecord::duration_us), std::uint32_t>);
static_assert(std::is_same_v<decltype(EventRecord::flags), std::uint32_t>);
static_assert(std::is_same_v<decltype(EventRecord::reserved), std::uint32_t>);
static_assert(noexcept(std::declval<SharedQueue<4>&>().try_write(std::declval<const EventRecord&>())));

static ScopeObservation succeeded_scope(ScopeToken token) {
    ScopeObservation scope{};
    scope.context_available = true;
    scope.query_result = S_OK;
    scope.token_count = 1;
    scope.tokens[0] = token;
    return scope;
}

class InputScopeFixture final : public ITfInputScope {
public:
    InputScopeFixture(std::initializer_list<InputScope> scopes)
        : scopes_(scopes) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_ITfInputScope)) {
            *out = static_cast<ITfInputScope *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = --refs_;
        if (!value) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE GetInputScopes(InputScope **scopes, UINT *count) override {
        if (!scopes || !count) return E_POINTER;
        *count = static_cast<UINT>(scopes_.size());
        *scopes = nullptr;
        if (scopes_.empty()) return S_OK;
        *scopes = static_cast<InputScope *>(CoTaskMemAlloc(
            sizeof(InputScope) * scopes_.size()));
        if (!*scopes) return E_OUTOFMEMORY;
        std::memcpy(*scopes, scopes_.data(), sizeof(InputScope) * scopes_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPhrase(BSTR **, UINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRegularExpression(BSTR *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSRGS(BSTR *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetXML(BSTR *) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> refs_{1};
    std::vector<InputScope> scopes_;
};

class ReadOnlyPropertyFixture final : public ITfReadOnlyProperty {
public:
    explicit ReadOnlyPropertyFixture(
        ITfInputScope *scope, HRESULT value_result = S_OK,
        VARTYPE variant_type = VT_UNKNOWN)
        : scope_(scope), value_result_(value_result),
          variant_type_(variant_type) {
        scope_->AddRef();
    }
    ~ReadOnlyPropertyFixture() { scope_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(id, IID_IUnknown)) {
            *out = static_cast<ITfReadOnlyProperty *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = --refs_;
        if (!value) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE GetType(GUID *type) override {
        if (!type) return E_POINTER;
        *type = GUID_PROP_INPUTSCOPE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetContext(ITfContext **context) override {
        if (!context) return E_POINTER;
        *context = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumRanges(
        TfEditCookie, IEnumTfRanges **ranges, ITfRange *) override {
        if (ranges) *ranges = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetValue(
        TfEditCookie, ITfRange *range, VARIANT *value) override {
        if (!range || !value) return E_POINTER;
        ++value_calls;
        VariantInit(value);
        if (FAILED(value_result_)) return value_result_;
        value->vt = variant_type_;
        if (variant_type_ == VT_UNKNOWN) {
            value->punkVal = scope_;
            scope_->AddRef();
        }
        return S_OK;
    }

    std::uint32_t value_calls = 0;

private:
    std::atomic<ULONG> refs_{1};
    ITfInputScope *scope_;
    HRESULT value_result_;
    VARTYPE variant_type_;
};

class InputScopeContextFixture final : public ITfContext {
public:
    InputScopeContextFixture(ITfContext *inner, ITfReadOnlyProperty *property)
        : inner_(inner), property_(property) {
        inner_->AddRef();
        property_->AddRef();
    }
    ~InputScopeContextFixture() {
        property_->Release();
        inner_->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_ITfContext)) {
            *out = static_cast<ITfContext *>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(id, IID_ITfInputScope)) ++direct_input_scope_queries;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = --refs_;
        if (!value) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE RequestEditSession(
        TfClientId client, ITfEditSession *session, DWORD flags, HRESULT *result) override {
        return inner_->RequestEditSession(client, session, flags, result);
    }
    HRESULT STDMETHODCALLTYPE InWriteSession(TfClientId client, BOOL *value) override {
        return inner_->InWriteSession(client, value);
    }
    HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie cookie, ULONG index,
        ULONG count, TF_SELECTION *selection, ULONG *fetched) override {
        return inner_->GetSelection(cookie, index, count, selection, fetched);
    }
    HRESULT STDMETHODCALLTYPE SetSelection(
        TfEditCookie cookie, ULONG count, const TF_SELECTION *selection) override {
        return inner_->SetSelection(cookie, count, selection);
    }
    HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie cookie, ITfRange **range) override {
        return inner_->GetStart(cookie, range);
    }
    HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie cookie, ITfRange **range) override {
        return inner_->GetEnd(cookie, range);
    }
    HRESULT STDMETHODCALLTYPE GetActiveView(ITfContextView **view) override {
        return inner_->GetActiveView(view);
    }
    HRESULT STDMETHODCALLTYPE EnumViews(IEnumTfContextViews **views) override {
        return inner_->EnumViews(views);
    }
    HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS *status) override {
        return inner_->GetStatus(status);
    }
    HRESULT STDMETHODCALLTYPE GetProperty(
        REFGUID property_id, ITfProperty **property) override {
        return inner_->GetProperty(property_id, property);
    }
    HRESULT STDMETHODCALLTYPE GetAppProperty(
        REFGUID property_id, ITfReadOnlyProperty **property) override {
        if (IsEqualGUID(property_id, GUID_PROP_INPUTSCOPE)) {
            ++app_property_queries;
            *property = property_;
            property_->AddRef();
            return S_OK;
        }
        return inner_->GetAppProperty(property_id, property);
    }
    HRESULT STDMETHODCALLTYPE TrackProperties(
        const GUID **properties, ULONG property_count,
        const GUID **app_properties, ULONG app_property_count,
        ITfReadOnlyProperty **property) override {
        return inner_->TrackProperties(properties, property_count,
            app_properties, app_property_count, property);
    }
    HRESULT STDMETHODCALLTYPE EnumProperties(IEnumTfProperties **properties) override {
        return inner_->EnumProperties(properties);
    }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr **document) override {
        return inner_->GetDocumentMgr(document);
    }
    HRESULT STDMETHODCALLTYPE CreateRangeBackup(
        TfEditCookie cookie, ITfRange *range, ITfRangeBackup **backup) override {
        return inner_->CreateRangeBackup(cookie, range, backup);
    }

    std::uint32_t app_property_queries = 0;
    std::uint32_t direct_input_scope_queries = 0;

private:
    std::atomic<ULONG> refs_{1};
    ITfContext *inner_;
    ITfReadOnlyProperty *property_;
};

class PropertyRangeFixture final : public IEnumTfRanges {
public:
    explicit PropertyRangeFixture(bool changed) : changed_(changed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(id, IID_IUnknown) ||
            IsEqualIID(id, IID_IEnumTfRanges)) {
            *out = static_cast<IEnumTfRanges *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = --refs_;
        if (!value) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE Clone(IEnumTfRanges **copy) override {
        if (!copy) return E_POINTER;
        *copy = new PropertyRangeFixture(changed_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Next(
        ULONG, ITfRange **range, ULONG *fetched) override {
        if (!range || !fetched) return E_POINTER;
        if (!changed_) {
            *range = nullptr;
            *fetched = 0;
            return S_FALSE;
        }
        *range = nullptr;
        *fetched = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Skip(ULONG) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    bool changed_;
};

class EditRecordFixture final : public ITfEditRecord {
public:
    explicit EditRecordFixture(bool changed) : changed_(changed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(id, IID_IUnknown) ||
            IsEqualIID(id, IID_ITfEditRecord)) {
            *out = static_cast<ITfEditRecord *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = --refs_;
        if (!value) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE GetSelectionStatus(BOOL *changed) override {
        if (!changed) return E_POINTER;
        *changed = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextAndPropertyUpdates(
        DWORD, const GUID **properties, ULONG count,
        IEnumTfRanges **ranges) override {
        if (!properties || !count || !ranges) return E_POINTER;
        if (!IsEqualGUID(*properties[0], GUID_PROP_INPUTSCOPE))
            return E_INVALIDARG;
        *ranges = new PropertyRangeFixture(changed_);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    bool changed_;
};

static int test_scope_classification() {
    CHECK(classify_scope(succeeded_scope(ScopeToken::Default)) == ScopeClass::Ordinary);
    CHECK(classify_scope(succeeded_scope(ScopeToken::Text)) == ScopeClass::Ordinary);
    CHECK(classify_scope(succeeded_scope(ScopeToken::Number)) == ScopeClass::Ordinary);
    CHECK(classify_scope(succeeded_scope(ScopeToken::Private)) == ScopeClass::Sensitive);
    CHECK(classify_scope(succeeded_scope(ScopeToken::Password)) == ScopeClass::Sensitive);
    CHECK(classify_scope(succeeded_scope(ScopeToken::Pin)) == ScopeClass::Sensitive);

    auto mixed_private = succeeded_scope(ScopeToken::Text);
    mixed_private.token_count = 2;
    mixed_private.tokens[1] = ScopeToken::Private;
    CHECK(classify_scope(mixed_private) == ScopeClass::Sensitive);

    auto disabled = succeeded_scope(ScopeToken::Default);
    disabled.context_disabled = true;
    CHECK(classify_scope(disabled) == ScopeClass::Sensitive);

    auto failed = succeeded_scope(ScopeToken::Default);
    failed.query_result = E_FAIL;
    CHECK(classify_scope(failed) == ScopeClass::Unknown);

    ScopeObservation missing{};
    missing.context_available = true;
    missing.query_result = S_OK;
    CHECK(classify_scope(missing) == ScopeClass::Unknown);

    missing = succeeded_scope(ScopeToken::Default);
    missing.context_available = false;
    CHECK(classify_scope(missing) == ScopeClass::Unknown);

    CHECK(classify_scope(succeeded_scope(ScopeToken::Unsupported)) == ScopeClass::Unknown);
    CHECK(normalize_input_scope(IS_DEFAULT) == ScopeToken::Default);
    CHECK(normalize_input_scope(IS_TEXT) == ScopeToken::Text);
    CHECK(normalize_input_scope(IS_NUMBER) == ScopeToken::Number);
    CHECK(normalize_input_scope(IS_PRIVATE) == ScopeToken::Private);
    CHECK(normalize_input_scope(IS_PASSWORD) == ScopeToken::Password);
    CHECK(normalize_input_scope(IS_NUMERIC_PIN) == ScopeToken::Pin);
    CHECK(normalize_input_scope(static_cast<InputScope>(999)) == ScopeToken::Unsupported);

    auto truncated = succeeded_scope(ScopeToken::Default);
    truncated.token_count = boshiamy::doctor::kScopeTokenCapacity + 1;
    CHECK(classify_scope(truncated) == ScopeClass::Unknown);
    return 0;
}

static int check_mapping_aces(PSECURITY_DESCRIPTOR descriptor,
    const std::wstring &user_sid, const std::wstring &package_sid) {
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    CHECK(GetSecurityDescriptorControl(descriptor, &control, &revision));
    CHECK((control & SE_DACL_PROTECTED) != 0);
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    CHECK(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted));
    CHECK(present && acl != nullptr);
    CHECK(acl->AceCount == (package_sid.empty() ? 1 : 2));
    bool found_user = false, found_package = false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void *raw = nullptr;
        CHECK(GetAce(acl, i, &raw));
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
        CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        CHECK((ace->Header.AceFlags & INHERITED_ACE) == 0);
        wchar_t *text = nullptr;
        CHECK(ConvertSidToStringSidW(
            const_cast<DWORD *>(&ace->SidStart), &text));
        const std::wstring sid(text);
        LocalFree(text);
        if (sid == user_sid) {
            CHECK(!found_user);
            CHECK(ace->Mask == (FILE_MAP_READ | FILE_MAP_WRITE));
            found_user = true;
        } else if (!package_sid.empty() && sid == package_sid) {
            CHECK(!found_package);
            CHECK(ace->Mask == (FILE_MAP_READ | FILE_MAP_WRITE | READ_CONTROL));
            found_package = true;
        } else {
            CHECK(false); // Reject broad or unrelated principals.
        }
    }
    CHECK(found_user && (package_sid.empty() || found_package));
    return 0;
}

static int test_user_only_security_descriptor() {
    CHECK(detail::local_mapping_name(1234, 0x5678) ==
        L"Local\\LiuKaiDoctorP0-1234-0000000000005678");
    CHECK(detail::local_mapping_name(1234, 0x5678).rfind(
        L"Local\\LiuKaiDoctorP0-", 0) == 0);
    const auto package_sddl = detail::mapping_security_sddl(
        L"S-1-5-21-1", L"S-1-15-2-123");
    PSECURITY_DESCRIPTOR package_descriptor = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        package_sddl.c_str(), SDDL_REVISION_1, &package_descriptor, nullptr));
    const int package_result = check_mapping_aces(
        package_descriptor, L"S-1-5-21-1", L"S-1-15-2-123");
    LocalFree(package_descriptor);
    CHECK(package_result == 0);
    auto security = UserSecurity::create_mapping();
    CHECK(SUCCEEDED(security.result));
    std::wstring user_sid;
    CHECK(detail::own_user_sid(&user_sid));
    std::wstring own_app_container_sid;
    detail::own_app_container_sid(&own_app_container_sid);
    CHECK(check_mapping_aces(security.descriptor, user_sid,
        own_app_container_sid) == 0);
    CHECK(detail::mutex_name(user_sid).find(user_sid) != std::wstring::npos);
    auto mutex_security = UserSecurity::create_mutex();
    CHECK(SUCCEEDED(mutex_security.result));
    HANDLE own_token = nullptr;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_token));
    std::vector<unsigned char> own_sid;
    DWORD own_session = 0;
    CHECK(SUCCEEDED(detail::token_sid_copy(own_token, &own_sid)));
    CHECK(SUCCEEDED(detail::token_session_id(own_token, &own_session)));
    CloseHandle(own_token);
    CHECK(detail::same_user_session(
        own_sid, own_session, own_sid, own_session));
    CHECK(!detail::same_user_session(
        own_sid, own_session, own_sid, own_session + 1));
    auto other_sid = own_sid;
    other_sid.back() ^= 1;
    CHECK(!detail::same_user_session(
        own_sid, own_session, other_sid, own_session));
    return 0;
}

static int test_property_scope_path_and_context_gate() {
    CHECK(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) == S_OK);
    ITfThreadMgr *manager = nullptr;
    CHECK(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgr, reinterpret_cast<void **>(&manager)) == S_OK);
    ITfThreadMgrEx *extended = nullptr;
    CHECK(manager->QueryInterface(IID_ITfThreadMgrEx,
        reinterpret_cast<void **>(&extended)) == S_OK);
    TfClientId client = 0;
    CHECK(extended->ActivateEx(
        &client, TF_TMAE_NOACTIVATETIP | TF_TMAE_UIELEMENTENABLEDONLY) == S_OK);
    extended->Release();

    ITfDocumentMgr *document = nullptr;
    ITfContext *inner = nullptr;
    TfEditCookie cookie = 0;
    CHECK(manager->CreateDocumentMgr(&document) == S_OK);
    CHECK(document->CreateContext(client, 0, nullptr, &inner, &cookie) == S_OK);
    CHECK(document->Push(inner) == S_OK);

    auto *ordinary_scope = new InputScopeFixture({IS_DEFAULT});
    auto *ordinary_property = new ReadOnlyPropertyFixture(ordinary_scope);
    auto *ordinary_context = new InputScopeContextFixture(inner, ordinary_property);
    auto *password_scope = new InputScopeFixture({IS_PASSWORD});
    auto *password_property = new ReadOnlyPropertyFixture(password_scope);
    auto *password_context = new InputScopeContextFixture(inner, password_property);
    auto *empty_scope = new InputScopeFixture({});
    auto *empty_property = new ReadOnlyPropertyFixture(empty_scope);
    auto *empty_context = new InputScopeContextFixture(inner, empty_property);
    auto *failed_scope = new InputScopeFixture({IS_DEFAULT});
    auto *failed_property = new ReadOnlyPropertyFixture(failed_scope, E_FAIL);
    auto *failed_context = new InputScopeContextFixture(inner, failed_property);
    auto *empty_variant_scope = new InputScopeFixture({IS_DEFAULT});
    auto *empty_variant_property = new ReadOnlyPropertyFixture(
        empty_variant_scope, S_OK, VT_EMPTY);
    auto *empty_variant_context = new InputScopeContextFixture(
        inner, empty_variant_property);
    ordinary_scope->Release();
    password_scope->Release();
    empty_scope->Release();
    failed_scope->Release();
    empty_variant_scope->Release();

    const auto ordinary = read_input_scope(client, ordinary_context);
    const auto sensitive = read_input_scope(client, password_context);
    const auto empty = read_input_scope(client, empty_context);
    const auto failed = read_input_scope(client, failed_context);
    const auto empty_variant = read_input_scope(client, empty_variant_context);
    CHECK(ordinary_context->app_property_queries == 1);
    CHECK(ordinary_context->direct_input_scope_queries == 0);
    CHECK(ordinary_property->value_calls == 1);
    CHECK(classify_scope(ordinary) == ScopeClass::Ordinary);
    CHECK(ordinary.app_property_hr == static_cast<std::int32_t>(S_OK));
    CHECK(ordinary.request_outer_hr == static_cast<std::int32_t>(S_OK));
    CHECK(ordinary.request_inner_hr == static_cast<std::int32_t>(S_OK));
    CHECK(ordinary.range_hr == static_cast<std::int32_t>(S_OK));
    CHECK(ordinary.property_value_hr == static_cast<std::int32_t>(S_OK));
    CHECK(ordinary.unpack_hr == static_cast<std::int32_t>(S_OK));
    CHECK(ordinary.variant_type == VT_UNKNOWN);
    CHECK(password_context->app_property_queries == 1);
    CHECK(password_context->direct_input_scope_queries == 0);
    CHECK(password_property->value_calls == 1);
    CHECK(classify_scope(sensitive) == ScopeClass::Sensitive);
    CHECK(empty_context->app_property_queries == 1);
    CHECK(empty.token_count == 0);
    CHECK(classify_scope(empty) == ScopeClass::Unknown);
    CHECK(failed_context->app_property_queries == 1);
    CHECK(failed.query_result == static_cast<std::int32_t>(E_FAIL));
    CHECK(failed.app_property_hr == static_cast<std::int32_t>(S_OK));
    CHECK(failed.request_outer_hr == static_cast<std::int32_t>(S_OK));
    CHECK(failed.request_inner_hr == static_cast<std::int32_t>(S_OK));
    CHECK(failed.property_value_hr == static_cast<std::int32_t>(E_FAIL));
    CHECK(classify_scope(failed) == ScopeClass::Unknown);
    CHECK(empty_variant.app_property_hr == static_cast<std::int32_t>(S_OK));
    CHECK(empty_variant.property_value_hr == static_cast<std::int32_t>(S_OK));
    CHECK(empty_variant.variant_type == VT_EMPTY);
    CHECK(empty_variant.unpack_hr == static_cast<std::int32_t>(S_FALSE));
    CHECK(empty_variant.query_result == static_cast<std::int32_t>(S_FALSE));
    CHECK(classify_scope(empty_variant) == ScopeClass::Unknown);
    ContextScopeCache empty_variant_cache;
    empty_variant_cache.replace(empty_variant_context, empty_variant);
    CHECK(!empty_variant_cache.needs_refresh(empty_variant_context));

    ContextScopeCache ordinary_gate;
    ContextScopeCache sensitive_gate;
    ordinary_gate.replace(ordinary_context, ordinary);
    sensitive_gate.replace(password_context, sensitive);
    const auto stale_generation = ordinary_gate.invalidate(ordinary_context);
    CHECK(!ordinary_gate.complete(
        ordinary_context, stale_generation + 1, sensitive));
    CHECK(ordinary_gate.complete(ordinary_context, stale_generation, ordinary));
    SharedQueue<4>::Storage shared_storage{};
    SharedQueue<4> shared(&shared_storage);
    CHECK(shared.initialize(0x1ULL, true));
    CHECK(shared.publish_scope(ordinary));
    CHECK(shared.scope_class() == ScopeClass::Ordinary);
    CHECK(ordinary_gate.classify(ordinary_context) == ScopeClass::Ordinary);
    CHECK(ordinary_gate.classify(password_context) == ScopeClass::Unknown);
    CHECK(sensitive_gate.classify(password_context) == ScopeClass::Sensitive);
    CHECK(sensitive_gate.classify(ordinary_context) == ScopeClass::Unknown);
    ContextScopeCache retry_cache;
    auto retry = failed;
    retry.query_result = static_cast<std::int32_t>(E_UNEXPECTED);
    retry_cache.replace(ordinary_context, retry);
    CHECK(retry_cache.needs_refresh(ordinary_context));
    CHECK(!ordinary_gate.needs_refresh(ordinary_context));

    auto *scope_edit = new EditRecordFixture(true);
    auto *ordinary_edit = new EditRecordFixture(false);
    CHECK(input_scope_property_changed(scope_edit));
    CHECK(!input_scope_property_changed(ordinary_edit));
    scope_edit->Release();
    ordinary_edit->Release();

    ordinary_gate.clear();
    sensitive_gate.clear();
    empty_variant_cache.clear();
    retry_cache.clear();
    ordinary_context->Release();
    password_context->Release();
    empty_context->Release();
    failed_context->Release();
    empty_variant_context->Release();
    ordinary_property->Release();
    password_property->Release();
    empty_property->Release();
    failed_property->Release();
    empty_variant_property->Release();
    CHECK(document->Pop(TF_POPF_ALL) == S_OK);
    inner->Release();
    document->Release();
    CHECK(manager->Deactivate() == S_OK);
    manager->Release();
    CoUninitialize();
    return 0;
}

static int test_live_writer_epoch_reclamation(void *producer_storage) {
    using Queue = SharedQueue<4>;
    Queue writer(producer_storage);
    Queue collector(producer_storage);
    CHECK(collector.initialize(0x1111222233334444ULL, true));
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> writes{0};
    std::thread thread([&] {
        EventRecord record{};
        record.scope = static_cast<std::uint32_t>(ScopeClass::Ordinary);
        while (running.load(std::memory_order_acquire)) {
            record.monotonic_ns = monotonic_nanoseconds();
            const auto result = writer.try_write(record);
            if (result == WriteResult::Written)
                writes.fetch_add(1, std::memory_order_relaxed);
            else
                SwitchToThread();
        }
    });
    for (std::size_t attempt = 0; attempt < 200 &&
            writes.load(std::memory_order_relaxed) == 0; ++attempt)
        Sleep(1);
    const bool writer_started = writes.load(std::memory_order_relaxed) != 0;
    const bool epoch_changed =
        collector.initialize(0x9999aaaabbbbccccULL, true);
    running.store(false, std::memory_order_release);
    thread.join();
    CHECK(writer_started);
    CHECK(epoch_changed);

    EventRecord read{};
    while (collector.try_read(&read) == ReadResult::Read) {
        CHECK(read.scope == static_cast<std::uint32_t>(ScopeClass::Ordinary));
    }
    CHECK(collector.set_enabled(true));
    for (std::uint64_t i = 0; i < 4; ++i) {
        EventRecord record{};
        record.operation_id = i;
        CHECK(writer.try_write(record) == WriteResult::Written);
    }
    const auto started = GetTickCount64();
    CHECK(writer.try_write(read) == WriteResult::Full);
    CHECK(GetTickCount64() - started < 100);
    for (std::uint64_t i = 0; i < 4; ++i)
        CHECK(collector.try_read(&read) == ReadResult::Read);
    return 0;
}

static int test_interrupted_reader_epoch_reclamation() {
    using Queue = SharedQueue<4>;
    Queue::Storage storage{};
    Queue queue(&storage);
    CHECK(queue.initialize(0xaaaaaaaa11111111ULL, true));
    EventRecord record{};
    record.scope = static_cast<std::uint32_t>(ScopeClass::Ordinary);
    CHECK(queue.try_write(record) == WriteResult::Written);

    CHECK(InterlockedCompareExchange64(
        &storage.control.read_index, 1, 0) == 0);
    CHECK(detail::read_i64(&storage.slots[0].sequence) == 1);

    CHECK(queue.initialize(0xbbbbbbbb22222222ULL, true));
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(queue.try_write(record) == WriteResult::Written);
    EventRecord read{};
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(queue.try_read(&read) == ReadResult::Read);
    CHECK(queue.try_read(&read) == ReadResult::Empty);
    return 0;
}

static int child_collector(int argc, char **argv) {
    if (argc != 5 || std::string(argv[1]) != "--collector") return 2;
    const DWORD pid = std::stoul(argv[2]);
    const std::uint64_t creation = std::stoull(argv[3], nullptr, 0);
    const std::uint64_t expected_sequence = std::stoull(argv[4], nullptr, 0);
    ProcessCollectorView collector(pid, creation);
    CHECK(collector.open());
    EventRecord record{};
    CHECK(collector.read(&record) == ReadResult::Read);
    CHECK(record.sequence == expected_sequence);
    return 0;
}

static int app_container_self_test() {
    ProcessProducer producer;
    CHECK(producer.ensure());
    ProcessProducer second_producer;
    CHECK(second_producer.ensure());
    CHECK(producer.producer_id() == second_producer.producer_id());
    ProcessCollectorView collector(
        GetCurrentProcessId(), producer.process_creation());
    CHECK(collector.open());
    CHECK(collector.initialize(0x4242424242424242ULL, true));
    EventRecord sent{};
    sent.operation_id = 1;
    sent.event_code = static_cast<std::uint32_t>(Event::Focus);
    sent.scope = static_cast<std::uint32_t>(ScopeClass::Unknown);
    CHECK(producer.try_write(sent) == WriteResult::Written);
    sent.operation_id = 2;
    CHECK(second_producer.try_write(sent) == WriteResult::Written);
    EventRecord read{};
    CHECK(collector.read(&read) == ReadResult::Read);
    CHECK(read.sequence == 1);
    CHECK(collector.read(&read) == ReadResult::Read);
    CHECK(read.sequence == 2);
    std::cout << "Doctor AppContainer self-open passed\n";
    return 0;
}

static int run_cross_process_read(ProcessProducer &producer) {
    ProcessCollectorView owner(GetCurrentProcessId(), producer.process_creation());
    CHECK(owner.open());
    CHECK(owner.initialize(0x2222222233333333ULL, true));

    EventRecord record{};
    record.operation_id = 77;
    record.event_code = static_cast<std::uint32_t>(Event::Focus);
    record.monotonic_ns = monotonic_nanoseconds();
    record.scope = static_cast<std::uint32_t>(ScopeClass::Ordinary);
    const auto expected_sequence = owner.snapshot().events_written + 1;
    CHECK(producer.try_write(record) == WriteResult::Written);

    char executable[MAX_PATH]{};
    CHECK(GetModuleFileNameA(nullptr, executable, static_cast<DWORD>(std::size(executable))) > 0);
    const std::string command = "\"" + std::string(executable) + "\" --collector " +
        std::to_string(GetCurrentProcessId()) + " " +
        std::to_string(static_cast<unsigned long long>(producer.process_creation())) + " " +
        std::to_string(expected_sequence);
    std::vector<char> command_line(command.begin(), command.end());
    command_line.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    CHECK(CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
        nullptr, nullptr, &startup, &child));
    CHECK(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD exit_code = 0xffffffffUL;
    CHECK(GetExitCodeProcess(child.hProcess, &exit_code));
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CHECK(exit_code == 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--appcontainer-self")
        return app_container_self_test();
    if (argc > 1) return child_collector(argc, argv);
    // Generated identities must survive the common signed-63-bit JSON contract.
    for (unsigned sample = 0; sample < 256; ++sample) {
        const auto id = detail::random_producer_id(0, 0);
        CHECK(id != 0 && id <= 0x7fffffffffffffffULL);
    }
    if (const int result = test_scope_classification()) return result;
    if (const int result = test_user_only_security_descriptor()) return result;
    if (const int result = test_property_scope_path_and_context_gate()) return result;
    if (const int result = test_interrupted_reader_epoch_reclamation()) return result;

    using Queue = SharedQueue<4>;
    static_assert(Queue::storage_bytes() <= 0xffffffffULL);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(Queue::storage_bytes()), nullptr);
    CHECK(mapping != nullptr);
    void *producer_storage = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, Queue::storage_bytes());
    void *collector_storage = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, Queue::storage_bytes());
    CHECK(producer_storage != nullptr && collector_storage != nullptr);

    Queue producer(producer_storage);
    Queue collector(collector_storage);
    CHECK(collector.initialize(0x123456789abcdef0ULL, true));

    EventRecord record{};
    record.producer_id = 0x0102030405060708ULL;
    record.operation_id = 0x1112131415161718ULL;
    record.sequence = 1;
    record.monotonic_ns = 123456789;
    record.schema_version = 1;
    record.event_code = 1;
    record.callback_source = 2;
    record.scope = static_cast<std::uint32_t>(ScopeClass::Ordinary);
    record.result_code = static_cast<std::int32_t>(S_OK);
    record.duration_us = 7;
    record.flags = 1;

    CHECK(producer.try_write(record) == WriteResult::Written);
    EventRecord read{};
    CHECK(collector.try_read(&read) == ReadResult::Read);
    EventRecord expected = record;
    expected.sequence = 1;
    CHECK(std::memcmp(&read, &expected, sizeof(expected)) == 0);
    CHECK(collector.try_read(&read) == ReadResult::Empty);

    SharedQueue<4, kProtocolVersion + 1> incompatible(producer_storage);
    CHECK(incompatible.try_write(record) == WriteResult::Invalid);

    CHECK(producer.try_write(record) == WriteResult::Written);
    CHECK(collector.initialize(0xabcdef0187654321ULL, true));
    CHECK(collector.try_read(&read) == ReadResult::Empty);
    record.sequence = 101;
    CHECK(producer.try_write(record) == WriteResult::Written);
    CHECK(collector.try_read(&read) == ReadResult::Read);
    CHECK(read.sequence == 3);

    for (std::uint64_t sequence = 2; sequence <= 5; ++sequence) {
        record.operation_id = sequence;
        CHECK(producer.try_write(record) == WriteResult::Written);
    }
    const auto started = GetTickCount64();
    CHECK(producer.try_write(record) == WriteResult::Full);
    CHECK(GetTickCount64() - started < 100);

    CHECK(collector.try_read(&read) == ReadResult::Read);
    CHECK(read.sequence == 4);
    CHECK(producer.try_write(record) == WriteResult::Written);

    CHECK(collector.set_enabled(false));
    CHECK(producer.try_write(record) == WriteResult::Disabled);

    if (const int result = test_live_writer_epoch_reclamation(producer_storage)) return result;

    ProcessProducer process_producer;
    CHECK(process_producer.ensure());
    ProcessCollectorView process_owner(GetCurrentProcessId(),
        process_producer.process_creation());
    CHECK(process_owner.open());
    CHECK(process_owner.initialize(0x3333444455556666ULL, true));
    ProcessCollectorView stale_process(
        GetCurrentProcessId(), process_producer.process_creation() ^ 1);
    CHECK(!stale_process.open());
    CHECK(stale_process.target_hr() ==
        HRESULT_FROM_WIN32(ERROR_RETRY));

    ProcessProducer second_process_producer;
    CHECK(second_process_producer.ensure());
    CHECK(process_producer.producer_id() == second_process_producer.producer_id());
    CHECK(process_producer.begin_per_key_operation(ScopeClass::Sensitive) == 0);
    CHECK(process_producer.begin_per_key_operation(ScopeClass::Unknown) == 0);
    const auto first_operation = process_producer.begin_operation();
    const auto second_operation = second_process_producer.begin_operation();
    CHECK(first_operation == 1);
    CHECK(second_operation == 2);
    record.operation_id = first_operation;
    CHECK(process_producer.try_write(record) == WriteResult::Written);
    record.operation_id = second_operation;
    CHECK(second_process_producer.try_write(record) == WriteResult::Written);
    CHECK(process_owner.read(&read) == ReadResult::Read);
    CHECK(read.operation_id == first_operation);
    const auto first_sequence = read.sequence;
    CHECK(read.producer_id == process_producer.producer_id());
    CHECK(process_owner.read(&read) == ReadResult::Read);
    CHECK(read.operation_id == second_operation);
    CHECK(read.sequence == first_sequence + 1);
    CHECK(read.producer_id == process_producer.producer_id());

    CHECK(process_owner.disable());
    const auto accepted_sequence = read.sequence;
    CHECK(process_producer.begin_operation() == 0);
    CHECK(process_producer.try_write(record) == WriteResult::Disabled);
    CHECK(second_process_producer.try_write(record) == WriteResult::Disabled);
    CHECK(process_owner.initialize(0x3333444455556666ULL, true));
    CHECK(process_producer.begin_operation() == first_operation + 2);
    CHECK(process_producer.try_write(record) == WriteResult::Written);
    CHECK(process_owner.read(&read) == ReadResult::Read);
    CHECK(read.sequence == accepted_sequence + 1);

    if (const int result = run_cross_process_read(process_producer)) return result;

    UnmapViewOfFile(collector_storage);
    UnmapViewOfFile(producer_storage);
    CloseHandle(mapping);
    std::cout << "Doctor transport, scope classification and bounded shared-memory queue passed\n";
    return 0;
}
