#ifndef BOSHIAMY_TSF_DOCTOR_SCOPE_HPP
#define BOSHIAMY_TSF_DOCTOR_SCOPE_HPP

#include "doctor_transport.hpp"
#include <inputscope.h>
#include <msctf.h>
#include "activation_diagnostics.hpp"
#include <objbase.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace boshiamy::doctor {

// Keep the input-scope GUID local to this header. Including initguid.h here
// changes DEFINE_GUID for every Windows header included by our callers.
inline constexpr GUID kInputScopeProperty = {
    0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

inline bool sensitive_input_scope(InputScope value) noexcept {
    return value == IS_PASSWORD || value == IS_PRIVATE ||
        value == IS_NUMERIC_PASSWORD || value == IS_NUMERIC_PIN ||
        value == IS_ALPHANUMERIC_PIN || value == IS_ALPHANUMERIC_PIN_SET;
}

inline bool numeric_input_scope(InputScope value) noexcept {
    return value == IS_CURRENCY_AMOUNTANDSYMBOL || value == IS_CURRENCY_AMOUNT ||
        value == IS_DATE_FULLDATE || value == IS_DATE_MONTH || value == IS_DATE_DAY ||
        value == IS_DATE_YEAR || value == IS_DIGITS || value == IS_NUMBER ||
        value == IS_NUMBER_FULLWIDTH || value == IS_CURRENCY_CHINESE ||
        value == IS_FORMULA_NUMBER || value == IS_FORMULA;
}

inline ScopeToken normalize_input_scope(InputScope value) noexcept {
    if (sensitive_input_scope(value))
        return value == IS_PRIVATE ? ScopeToken::Private :
            (value == IS_PASSWORD || value == IS_NUMERIC_PASSWORD ?
                ScopeToken::Password : ScopeToken::Pin);
    if (value == IS_DEFAULT) return ScopeToken::Default;
    if (value >= IS_DEFAULT && value <= IS_CHAT_WITHOUT_EMOJI)
        return numeric_input_scope(value) ? ScopeToken::Number : ScopeToken::Text;
    return ScopeToken::Unsupported;
}

inline HRESULT unpack_input_scope(const VARIANT &value, InputScope **scopes,
    UINT *count) noexcept {
    if (!scopes || !count) return E_POINTER;
    *scopes = nullptr;
    *count = 0;
    if (value.vt == VT_EMPTY) return S_FALSE;
    IUnknown *unknown = nullptr;
    bool release_unknown = false;
    if (value.vt == VT_UNKNOWN) {
        unknown = value.punkVal;
    } else if (value.vt == VT_DISPATCH && value.pdispVal) {
        const HRESULT hr = value.pdispVal->QueryInterface(
            IID_IUnknown, reinterpret_cast<void **>(&unknown));
        if (FAILED(hr)) return hr;
        release_unknown = true;
    }
    if (!unknown) return E_NOINTERFACE;
    ITfInputScope *input_scope = nullptr;
    HRESULT hr = unknown->QueryInterface(
        __uuidof(ITfInputScope), reinterpret_cast<void **>(&input_scope));
    if (release_unknown) unknown->Release();
    if (FAILED(hr) || !input_scope)
        return FAILED(hr) ? hr : E_NOINTERFACE;
    InputScope *raw = nullptr;
    UINT raw_count = 0;
    hr = input_scope->GetInputScopes(&raw, &raw_count);
    input_scope->Release();
    if (FAILED(hr)) {
        if (raw) CoTaskMemFree(raw);
        return hr;
    }
    if (!raw || raw_count == 0) {
        if (raw) CoTaskMemFree(raw);
        return S_FALSE;
    }
    *scopes = raw;
    *count = raw_count;
    return S_OK;
}

inline ScopeObservation read_scope_with_cookie(
    ITfContext *context, TfEditCookie cookie) noexcept {
    ScopeObservation result;
    result.context_available = context != nullptr;
    if (!context) {
        result.query_result = static_cast<std::int32_t>(E_INVALIDARG);
        return result;
    }

    const auto gates = tsf_activation::read_context_gates(context);
    result.context_disabled =
        tsf_activation::classify_context(gates) ==
        tsf_activation::ContextDisposition::Blocked;
    if (result.context_disabled) {
        result.query_result = static_cast<std::int32_t>(S_OK);
        return result;
    }

    ITfReadOnlyProperty *property = nullptr;
    HRESULT hr = context->GetAppProperty(kInputScopeProperty, &property);
    result.app_property_hr = static_cast<std::int32_t>(hr);
    if (FAILED(hr) || !property) {
        if (property) property->Release();
        result.query_result = static_cast<std::int32_t>(
            FAILED(hr) ? hr : E_NOINTERFACE);
        return result;
    }

    TF_SELECTION selection{};
    ULONG fetched = 0;
    hr = context->GetSelection(
        cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    ITfRange *range = selection.range;
    if (FAILED(hr) || fetched == 0 || !range) {
        if (range) range->Release();
        range = nullptr;
        hr = context->GetStart(cookie, &range);
    }
    result.range_hr = static_cast<std::int32_t>(
        FAILED(hr) || !range ? (FAILED(hr) ? hr : S_FALSE) : S_OK);
    if (FAILED(hr) || !range) {
        if (range) range->Release();
        property->Release();
        result.query_result = result.range_hr;
        return result;
    }

    VARIANT value;
    VariantInit(&value);
    hr = property->GetValue(cookie, range, &value);
    range->Release();
    property->Release();
    result.property_value_hr = static_cast<std::int32_t>(hr);
    result.variant_type = value.vt;
    if (FAILED(hr)) {
        VariantClear(&value);
        result.query_result = static_cast<std::int32_t>(hr);
        return result;
    }

    InputScope *raw = nullptr;
    UINT count = 0;
    hr = unpack_input_scope(value, &raw, &count);
    result.unpack_hr = static_cast<std::int32_t>(hr);
    result.query_result = static_cast<std::int32_t>(hr);
    if (SUCCEEDED(hr)) {
        result.token_count = count;
        result.truncated = count > kScopeTokenCapacity;
        const UINT copied = count > kScopeTokenCapacity
            ? static_cast<UINT>(kScopeTokenCapacity) : count;
        for (UINT i = 0; i < copied; ++i) {
            result.raw_tokens[i] = static_cast<std::uint32_t>(raw[i]);
            result.tokens[i] = normalize_input_scope(raw[i]);
        }
        CoTaskMemFree(raw);
    }
    VariantClear(&value);
    return result;
}

using ScopeReadCompletion = std::function<void(
    ITfContext *, std::uint64_t, const ScopeObservation &)>;

struct ScopeRequestResult {
    HRESULT outer = E_PENDING;
    HRESULT inner = E_PENDING;
};

class ScopeReadSession final : public ITfEditSession {
public:
    ScopeReadSession(ITfContext *context, std::uint64_t generation,
        ScopeObservation *synchronous_result,
        ScopeReadCompletion completion = {},
        std::shared_ptr<void> lifetime = {})
        : context_(context), generation_(generation),
          synchronous_result_(synchronous_result),
          completion_(std::move(completion)),
          lifetime_(std::move(lifetime)) {
        context_->AddRef();
    }
    ~ScopeReadSession() { context_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(id, IID_IUnknown) &&
            !IsEqualIID(id, IID_ITfEditSession))
            return E_NOINTERFACE;
        *out = static_cast<ITfEditSession *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = --refs_;
        if (!value) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        auto result = read_scope_with_cookie(context_, cookie);
        result.request_outer_hr = static_cast<std::int32_t>(S_OK);
        result.request_inner_hr = static_cast<std::int32_t>(S_OK);
        if (synchronous_result_) *synchronous_result_ = result;
        if (completion_)
            completion_(context_, generation_, result);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    ITfContext *context_;
    std::uint64_t generation_;
    ScopeObservation *synchronous_result_;
    ScopeReadCompletion completion_;
    std::shared_ptr<void> lifetime_;
};

inline ScopeObservation read_input_scope(TfClientId client_id,
    ITfContext *context) noexcept {
    ScopeObservation result;
    result.context_available = context != nullptr;
    if (!context) {
        result.query_result = static_cast<std::int32_t>(E_INVALIDARG);
        return result;
    }
    auto *session = new(std::nothrow) ScopeReadSession(
        context, 0, &result);
    if (!session) {
        result.query_result = static_cast<std::int32_t>(E_OUTOFMEMORY);
        return result;
    }
    HRESULT session_result = E_UNEXPECTED;
    const HRESULT request = context->RequestEditSession(
        client_id, session, TF_ES_SYNC | TF_ES_READ, &session_result);
    session->Release();
    result.request_outer_hr = static_cast<std::int32_t>(request);
    if (FAILED(request)) result.query_result = static_cast<std::int32_t>(request);
    else {
        result.request_inner_hr = static_cast<std::int32_t>(session_result);
        if (FAILED(session_result))
            result.query_result = static_cast<std::int32_t>(session_result);
    }
    return result;
}

inline HRESULT request_input_scope(TfClientId client_id, ITfContext *context,
    std::uint64_t generation, ScopeReadCompletion completion,
    ScopeRequestResult *status = nullptr,
    std::shared_ptr<void> lifetime = {}) noexcept {
    if (!context) {
        if (status) status->outer = E_INVALIDARG;
        return E_INVALIDARG;
    }
    auto *session = new(std::nothrow) ScopeReadSession(
        context, generation, nullptr, std::move(completion),
        std::move(lifetime));
    if (!session) {
        if (status) status->outer = E_OUTOFMEMORY;
        return E_OUTOFMEMORY;
    }
    HRESULT session_result = E_PENDING;
    const HRESULT request = context->RequestEditSession(
        client_id, session, TF_ES_ASYNC | TF_ES_READ, &session_result);
    if (status) {
        status->outer = request;
        status->inner = session_result;
    }
    session->Release();
    if (FAILED(request)) return request;
    if (FAILED(session_result) && session_result != E_PENDING)
        return session_result;
    return S_OK;
}

inline ScopeObservation read_input_scope_with_cookie(
    ITfContext *context, TfEditCookie cookie) noexcept {
    auto result = read_scope_with_cookie(context, cookie);
    result.request_outer_hr = static_cast<std::int32_t>(S_OK);
    result.request_inner_hr = static_cast<std::int32_t>(S_OK);
    return result;
}

inline ITfContext *focused_context(ITfThreadMgr *manager) noexcept {
    if (!manager) return nullptr;
    ITfDocumentMgr *document = nullptr;
    HRESULT hr = manager->GetFocus(&document);
    if (FAILED(hr) || !document) return nullptr;
    ITfContext *context = nullptr;
    hr = document->GetTop(&context);
    document->Release();
    if (FAILED(hr)) {
        if (context) context->Release();
        return nullptr;
    }
    return context;
}

inline ITfContext *document_context(ITfDocumentMgr *document) noexcept {
    if (!document) return nullptr;
    ITfContext *context = nullptr;
    const HRESULT hr = document->GetTop(&context);
    if (FAILED(hr)) {
        if (context) context->Release();
        return nullptr;
    }
    return context;
}

inline bool input_scope_property_changed(ITfEditRecord *record) noexcept {
    if (!record) return false;
    const GUID *properties[] = {&kInputScopeProperty};
    IEnumTfRanges *ranges = nullptr;
    const HRESULT query = record->GetTextAndPropertyUpdates(
        0, properties, 1, &ranges);
    if (FAILED(query) || !ranges) {
        if (ranges) ranges->Release();
        return false;
    }
    ITfRange *range = nullptr;
    ULONG fetched = 0;
    const HRESULT next = ranges->Next(1, &range, &fetched);
    const bool changed = SUCCEEDED(next) && fetched != 0;
    if (range) range->Release();
    ranges->Release();
    return changed;
}

class ContextScopeCache final {
public:
    ContextScopeCache() = default;
    ContextScopeCache(const ContextScopeCache &) = delete;
    ContextScopeCache &operator=(const ContextScopeCache &) = delete;
    ~ContextScopeCache() { clear(); }

    std::uint64_t invalidate(ITfContext *context) noexcept {
        ++generation_;
        if (context) context->AddRef();
        if (context_) context_->Release();
        context_ = context;
        observation_ = {};
        observation_.context_available = context != nullptr;
        observation_.query_result = static_cast<std::int32_t>(E_PENDING);
        return generation_;
    }

    void replace(ITfContext *context, const ScopeObservation &observation) noexcept {
        invalidate(context);
        observation_ = observation;
    }

    bool complete(ITfContext *context, std::uint64_t generation,
        const ScopeObservation &observation) noexcept {
        if (generation != generation_ || !context_ || context != context_)
            return false;
        observation_ = observation;
        return true;
    }

    void clear() noexcept {
        ++generation_;
        if (context_) context_->Release();
        context_ = nullptr;
        observation_ = {};
    }

    bool is_context(ITfContext *context) const noexcept {
        return context_ && context == context_;
    }

    bool needs_refresh(ITfContext *context) const noexcept {
        if (!context_ || context != context_) return true;
        const auto hr = static_cast<HRESULT>(observation_.query_result);
        return hr == E_PENDING || hr == E_UNEXPECTED;
    }

    ScopeClass classify(ITfContext *context) const noexcept {
        if (!context)
            return classify_scope(observation_);
        if (!context_ || context != context_)
            return ScopeClass::Unknown;
        return classify_scope(observation_);
    }

    const ScopeObservation &observation() const noexcept {
        return observation_;
    }

    std::uint64_t generation() const noexcept { return generation_; }

private:
    ITfContext *context_ = nullptr;
    ScopeObservation observation_{};
    std::uint64_t generation_ = 0;
};

} // namespace boshiamy::doctor

#endif
