#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <msctf.h>
#include <atomic>
#include <fstream>
#include <memory>
#include <string>
#include <filesystem>
#include "../dictionary.hpp"
#include "../keyboard_modes.hpp"
#include "candidate_window.hpp"
#include "candidate_ui_element.hpp"
#include "language_bar.hpp"
#include "text_layout.hpp"
#include "../shift_toggle.hpp"
#include "../settings.hpp"
#include "preserved_keys.hpp"
#include "open_close_binding.hpp"
#include "compartment_ids.hpp"
#include "activation_diagnostics.hpp"
#include "doctor_scope.hpp"
#include "../build_info.hpp"
#include "../doctor_paths.hpp"

static const CLSID CLSID_BoshiamyTsf={0x5d7c4291,0x2b34,0x4ce8,{0x8a,0x11,0x39,0x62,0x71,0x05,0xc4,0x2e}};
static const GUID GUID_BoshiamyProfile={0x7254a21f,0x6eb1,0x4217,{0xa4,0x2c,0xc2,0x73,0x01,0x17,0x5d,0x90}};
static std::atomic<long> objects{0}, locks{0};
struct ModuleObjectLifetime {ModuleObjectLifetime(){++objects;}~ModuleObjectLifetime(){--objects;}};
static HMODULE dll_module() {
 HMODULE self=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&CLSID_BoshiamyTsf),&self);return self;
}
static std::filesystem::path dll_directory() {
 wchar_t path[MAX_PATH]{};HMODULE self=dll_module();DWORD n=self?GetModuleFileNameW(self,path,MAX_PATH):0;
 return n?std::filesystem::path(path).parent_path():std::filesystem::path();
}
static std::unique_ptr<boshiamy::Engine> load_engine() {
 try {std::ifstream f(dll_directory()/L"dictionary.tsv",std::ios::binary);if(!f)return {};auto d=boshiamy::load_dictionary(f);auto engine=std::make_unique<boshiamy::Engine>(d.make_engine());
  std::ifstream readings(dll_directory()/L"readings.tsv",std::ios::binary);
  if(readings){try{engine->set_readings(boshiamy::Readings::parse(readings));}catch(...){/* Keep ordinary input available if the optional reading model cannot load. */}}
  std::ifstream phonetic_candidates(dll_directory()/L"phonetic-candidates.tsv",std::ios::binary);
  if(phonetic_candidates){try{engine->set_phonetic_candidates(boshiamy::PhoneticCandidateIndex::parse(phonetic_candidates));}catch(...){/* Keep ordinary input and legacy readings available if the optional ordered index cannot load. */}}
  engine->set_observed_primary_homophones(boshiamy::ObservedPrimaryHomophoneIndex::built_in());
  std::ifstream recommendations(dll_directory()/L"recommended-codes.tsv",std::ios::binary);
  if(recommendations){try{engine->set_recommendations(boshiamy::RecommendedCodes::parse(recommendations));}catch(...){/* Unknown recommendations remain absent; never infer from shortest codes. */}}
  return engine;}
 catch(...){return {};}
}
static std::wstring to_wide(const std::string& s) {
 if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);if(n<=0)return {};
 std::wstring out((size_t)n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),out.data(),n);return out;
}
static HRESULT launch_tool_process_arguments(const std::filesystem::path& executable,const std::wstring& argument){
 std::wstring command=L"\""+executable.wstring()+L"\"";
 if(!argument.empty())command+=L" "+argument;
 STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
 auto directory=executable.parent_path();
 if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&startup,&process))return HRESULT_FROM_WIN32(GetLastError());
 CloseHandle(process.hThread);CloseHandle(process.hProcess);return S_OK;
}
static HRESULT launch_tool_process(const std::filesystem::path& executable){return launch_tool_process_arguments(executable,L"");}
class Service;
using DoctorScopeReader=std::function<boshiamy::doctor::ScopeObservation(
 ITfThreadMgr*,TfClientId,ITfContext**)>;
class CompositionSink final: public ITfCompositionSink {
 std::atomic<ULONG> refs{1}; Service* owner;
public: explicit CompositionSink(Service* s); ~CompositionSink();
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,IID_ITfCompositionSink))*out=this;else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;} ULONG STDMETHODCALLTYPE Release() override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie,ITfComposition*) override;
};
class EditRequest final: public ITfEditSession {
 std::atomic<ULONG> refs{1}; Service* owner; ITfContext* context; boshiamy::Outcome outcome;boshiamy::Engine accepted_engine;std::uint64_t doctor_operation_id=0;
public: EditRequest(Service*,ITfContext*,const boshiamy::Outcome&,boshiamy::Engine&&); ~EditRequest();
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override; ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;} ULONG STDMETHODCALLTYPE Release() override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie) override;
 HRESULT apply(TfEditCookie);
};
class LayoutRefresh final: public ITfEditSession {
 std::atomic<ULONG> refs{1};Service* owner;ITfContext* context;std::size_t generation;
public:
 LayoutRefresh(Service*,ITfContext*,std::size_t);~LayoutRefresh();
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,IID_ITfEditSession))*out=this;else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override;
};
class Service final: public ITfTextInputProcessorEx, public ITfKeyEventSink,
 public ITfTextLayoutSink, public ITfThreadMgrEventSink, public ITfTextEditSink {
 struct DoctorScopeLifetime {Service* owner=nullptr;};
 boshiamy::doctor::ProcessProducer doctor;
 boshiamy::doctor::ContextScopeCache doctor_scope_cache;
 DoctorScopeReader doctor_scope_reader;
 std::shared_ptr<DoctorScopeLifetime> doctor_scope_lifetime;
 std::shared_ptr<void> doctor_scope_pin;
 std::uint64_t doctor_operation_id=0;
 std::uint64_t doctor_pending_test_operation=0;
 ITfContext* doctor_pending_test_context=nullptr;
 std::uint64_t doctor_pending_release_operation=0;
 ITfContext* doctor_pending_release_context=nullptr;
 bool doctor_composing=false;
 bool doctor_context_blocked=false;
 bool doctor_context_gate_blocked=false;
 boshiamy::OpenCloseBinding doctor_keyboard_disabled;
 boshiamy::OpenCloseBinding doctor_empty_context;
 ITfSource* doctor_thread_source=nullptr;DWORD doctor_thread_cookie=TF_INVALID_COOKIE;
 ITfSource* doctor_context_source=nullptr;ITfContext* doctor_context=nullptr;
 ITfReadOnlyProperty* doctor_tracked_property=nullptr;DWORD doctor_context_cookie=TF_INVALID_COOKIE;
 std::uint32_t activation_entry_point=0;DWORD activation_argument_flags=0;bool ui_elements_only_activation=false;
 void doctor_scope_completed(ITfContext* context,std::uint64_t generation,const boshiamy::doctor::ScopeObservation& scope) noexcept {
  if(!doctor_scope_cache.complete(context,generation,scope))return;
  if(doctor_context_gate_blocked&&!scope.context_disabled){
   doctor_observe_blocked_context(context);
   return;
  }
  doctor_context_blocked=doctor_context_gate_blocked||scope.context_disabled;
  doctor.publish_scope(scope);
 }
 void doctor_request_scope(ITfContext* context) noexcept {
  if(!doctor.ensure())return;
  if(!context){clear_doctor_scope();return;}
  if(doctor_context_gate_blocked){
   doctor_observe_blocked_context(context);
   return;
  }
  const auto generation=doctor_scope_cache.invalidate(context);
  doctor_context_blocked=doctor_context_gate_blocked;
  doctor.publish_scope(doctor_scope_cache.observation());
  try{
   if(doctor_scope_reader){
    ITfContext* reader_context=nullptr;
    auto scope=doctor_scope_reader(mgr,client,&reader_context);
    if(reader_context)reader_context->Release();
    doctor_scope_completed(context,generation,scope);
   }
   else{
    std::weak_ptr<DoctorScopeLifetime> lifetime=doctor_scope_lifetime;
    boshiamy::doctor::ScopeRequestResult request_status;
    const auto hr=boshiamy::doctor::request_input_scope(client,context,generation,
     [lifetime](ITfContext* completed,std::uint64_t completed_generation,const boshiamy::doctor::ScopeObservation& scope){
      auto target=lifetime.lock();if(target&&target->owner)target->owner->doctor_scope_completed(completed,completed_generation,scope);
     },&request_status,doctor_scope_pin);
    if(FAILED(hr)){
     boshiamy::doctor::ScopeObservation failed;
     failed.context_available=true;failed.query_result=static_cast<std::int32_t>(hr);
     failed.request_outer_hr=static_cast<std::int32_t>(request_status.outer);
     failed.request_inner_hr=static_cast<std::int32_t>(request_status.inner);
     doctor_scope_completed(context,generation,failed);
    }else if(static_cast<HRESULT>(
        doctor_scope_cache.observation().query_result)==E_PENDING){
     auto pending=doctor_scope_cache.observation();
     pending.request_outer_hr=static_cast<std::int32_t>(request_status.outer);
     pending.request_inner_hr=static_cast<std::int32_t>(request_status.inner);
     doctor_scope_completed(context,generation,pending);
    }
   }
  }catch(...){
   boshiamy::doctor::ScopeObservation failed;
   failed.context_available=true;failed.query_result=static_cast<std::int32_t>(E_UNEXPECTED);
   failed.request_outer_hr=static_cast<std::int32_t>(E_UNEXPECTED);
   doctor_scope_completed(context,generation,failed);
  }
 }
 std::function<void()> doctor_gate_callback() {
  std::weak_ptr<DoctorScopeLifetime> lifetime=doctor_scope_lifetime;
  return [lifetime](){
   auto target=lifetime.lock();
   if(target&&target->owner)target->owner->doctor_context_gate_changed();
  };
 }
 static bool doctor_gate_flag_readable(
  const boshiamy::tsf_activation::ContextFlag& flag) noexcept {
  if(flag.enumerate_result!=S_OK&&flag.enumerate_result!=S_FALSE)return false;
  return !flag.present||
   flag.value_result==S_OK||flag.value_result==S_FALSE;
 }
 static bool doctor_gate_readable(
  const boshiamy::tsf_activation::ContextGates& gates) noexcept {
  return gates.manager_query_result==S_OK&&
   gates.enumeration_result==S_OK&&
   doctor_gate_flag_readable(gates.keyboard_disabled)&&
   doctor_gate_flag_readable(gates.empty_context);
 }
 static bool doctor_gate_blocked(
  const boshiamy::tsf_activation::ContextGates& gates) noexcept {
  return (gates.keyboard_disabled.known&&
    gates.keyboard_disabled.value)||
   (gates.empty_context.known&&gates.empty_context.value);
 }
 void doctor_detach_gate_bindings() noexcept {
  doctor_keyboard_disabled.detach();
  doctor_empty_context.detach();
 }
 void doctor_attach_gate_bindings(ITfContext* context) noexcept {
  doctor_detach_gate_bindings();
  doctor_context_gate_blocked=false;
  ITfCompartmentMgr* compartments=nullptr;
  HRESULT hr=context?context->QueryInterface(
   IID_ITfCompartmentMgr,reinterpret_cast<void**>(&compartments)):E_POINTER;
  ITfCompartment* disabled=nullptr;
  ITfCompartment* empty=nullptr;
  if(SUCCEEDED(hr))
   hr=compartments->GetCompartment(
    GUID_COMPARTMENT_KEYBOARD_DISABLED,&disabled);
  if(SUCCEEDED(hr))
   hr=compartments->GetCompartment(
    GUID_COMPARTMENT_EMPTYCONTEXT,&empty);
  try{
   auto callback=doctor_gate_callback();
   if(SUCCEEDED(hr))
    hr=doctor_keyboard_disabled.attach(
     disabled,client,callback,doctor_scope_pin,
     GUID_COMPARTMENT_KEYBOARD_DISABLED);
   if(SUCCEEDED(hr))
    hr=doctor_empty_context.attach(
     empty,client,std::move(callback),doctor_scope_pin,
     GUID_COMPARTMENT_EMPTYCONTEXT);
  }catch(...){
   hr=E_OUTOFMEMORY;
  }
  if(disabled)disabled->Release();
  if(empty)empty->Release();
  if(compartments)compartments->Release();
  if(FAILED(hr)){
   doctor_detach_gate_bindings();
   doctor_context_gate_blocked=true;
   doctor_observe_blocked_context(context);
   return;
  }
  const auto gates=boshiamy::tsf_activation::read_context_gates(context);
  if(!doctor_gate_readable(gates)||doctor_gate_blocked(gates)){
   doctor_context_gate_blocked=true;
   doctor_observe_blocked_context(context);
  }
 }
 void doctor_context_gate_changed() noexcept {
  if(!doctor_context)return;
  ServicePin pin(this);
  const auto gates=boshiamy::tsf_activation::read_context_gates(
   doctor_context);
  if(!doctor_gate_readable(gates)||doctor_gate_blocked(gates)){
   doctor_context_gate_blocked=true;
   doctor_observe_blocked_context(doctor_context);
   return;
  }
  doctor_context_gate_blocked=false;
  doctor_request_scope(doctor_context);
 }
 void doctor_detach_context() noexcept {
  doctor_detach_gate_bindings();
  if(doctor_context_source&&doctor_context_cookie!=TF_INVALID_COOKIE){
   doctor_context_source->UnadviseSink(doctor_context_cookie);
  }
  if(doctor_context_source){doctor_context_source->Release();doctor_context_source=nullptr;}
  doctor_context_cookie=TF_INVALID_COOKIE;
  if(doctor_tracked_property){doctor_tracked_property->Release();doctor_tracked_property=nullptr;}
  if(doctor_context){doctor_context->Release();doctor_context=nullptr;}
 }
 void doctor_attach_context(ITfContext* context,bool request_scope=true) noexcept {
  if(context&&!doctor_scope_cache.is_context(context))doctor_scope_cache.invalidate(context);
  if(!context){doctor_detach_context();doctor_request_scope(nullptr);return;}
  if(!doctor_context||!same_context(doctor_context,context)){
   doctor_detach_context();
   doctor_context=context;doctor_context->AddRef();
   doctor_attach_gate_bindings(context);
   doctor_context->QueryInterface(IID_ITfSource,reinterpret_cast<void**>(&doctor_context_source));
   const GUID* tracked[]={&GUID_PROP_INPUTSCOPE};
   doctor_context->TrackProperties(nullptr,0,tracked,1,&doctor_tracked_property);
   if(doctor_context_source){
    doctor_context_source->AdviseSink(IID_ITfTextEditSink,
     static_cast<ITfTextEditSink*>(this),&doctor_context_cookie);
   }
  }
  if(request_scope)doctor_request_scope(context);
 }
 void doctor_advise_thread_events() noexcept {
  if(!mgr||doctor_thread_source)return;
  mgr->QueryInterface(IID_ITfSource,reinterpret_cast<void**>(&doctor_thread_source));
  if(doctor_thread_source&&FAILED(doctor_thread_source->AdviseSink(
      IID_ITfThreadMgrEventSink,static_cast<ITfThreadMgrEventSink*>(this),
      &doctor_thread_cookie))){
   doctor_thread_source->Release();doctor_thread_source=nullptr;
   doctor_thread_cookie=TF_INVALID_COOKIE;
  }
 }
 void doctor_unadvise_thread_events() noexcept {
  if(doctor_thread_source&&doctor_thread_cookie!=TF_INVALID_COOKIE)
   doctor_thread_source->UnadviseSink(doctor_thread_cookie);
  if(doctor_thread_source)doctor_thread_source->Release();
  doctor_thread_source=nullptr;doctor_thread_cookie=TF_INVALID_COOKIE;
 }
 void refresh_doctor_scope() noexcept {
  if(!doctor.ensure())return;
  ITfContext* context=boshiamy::doctor::focused_context(mgr);
  doctor_attach_context(context);
  if(context)context->Release();
 }
 void clear_doctor_scope() noexcept {
  doctor_pending_test_operation=0;doctor_pending_test_context=nullptr;
  doctor_pending_release_operation=0;doctor_pending_release_context=nullptr;
  doctor_detach_context();
  doctor_scope_cache.clear();
  doctor_context_blocked=false;
  doctor_context_gate_blocked=false;
  doctor.publish_scope(doctor_scope_cache.observation());
 }
 boshiamy::doctor::ScopeClass doctor_scope_for_context(ITfContext* context) const noexcept {
  if(doctor_context_gate_blocked||doctor_context_blocked)
   return boshiamy::doctor::ScopeClass::Sensitive;
  return doctor_scope_cache.classify(context);
 }
 std::uint64_t begin_doctor_operation(ITfContext* context) noexcept {
  doctor_operation_id=doctor.begin_per_key_operation(
   doctor_scope_for_context(context));
  return doctor_operation_id;
 }
 void continue_doctor_operation(ITfContext* context,std::uint64_t& pending,ITfContext*& pending_context) noexcept {
  const auto operation=pending;const auto* tested_context=pending_context;
  pending=0;pending_context=nullptr;
  if(operation&&tested_context==context&&doctor_scope_for_context(context)==boshiamy::doctor::ScopeClass::Ordinary)
   doctor_operation_id=operation;
  else begin_doctor_operation(context);
 }
 void doctor_observe_blocked_context(ITfContext* context) noexcept {
  doctor_pending_test_operation=0;doctor_pending_test_context=nullptr;
  doctor_pending_release_operation=0;doctor_pending_release_context=nullptr;
  doctor_context_blocked=true;
  doctor_context_gate_blocked=true;
  doctor_operation_id=0;
  boshiamy::doctor::ScopeObservation blocked;
  blocked.context_available=true;blocked.context_disabled=true;
  blocked.query_result=static_cast<std::int32_t>(E_PENDING);
  blocked.app_property_hr=static_cast<std::int32_t>(S_OK);
  if(context){
   const auto generation=doctor_scope_cache.invalidate(context);
   doctor_scope_cache.complete(context,generation,blocked);
  }
  doctor.publish_scope(blocked);
 }
 static bool doctor_per_key_event(boshiamy::doctor::Event event) noexcept {
  switch(event){
   case boshiamy::doctor::Event::TestDown:
   case boshiamy::doctor::Event::KeyDown:
   case boshiamy::doctor::Event::EditRequest:
   case boshiamy::doctor::Event::EditApplied:
   case boshiamy::doctor::Event::ShiftTestUp:
    case boshiamy::doctor::Event::ExtendedInterfaceQuery:
   case boshiamy::doctor::Event::ShiftUp:return true;
   default:return false;
  }
 }
 static std::uint32_t doctor_elapsed_us(std::uint64_t started) noexcept {
  const auto now=boshiamy::doctor::monotonic_nanoseconds();
  const auto elapsed=now>=started?(now-started)/1000:0;
  return elapsed>0xffffffffULL?0xffffffffu:static_cast<std::uint32_t>(elapsed);
 }
 std::uint64_t trace(boshiamy::doctor::Event event,ITfContext* context=nullptr,HRESULT result=S_OK,HRESULT session=S_FALSE,BOOL eaten=FALSE,ITfThreadMgr* inspection_manager=nullptr,std::uint64_t operation_id=0,std::uint32_t duration_us=0) noexcept {
  (void)context;(void)inspection_manager;
  const auto scope=doctor_scope_for_context(context);
  if(doctor_per_key_event(event)&&(scope!=boshiamy::doctor::ScopeClass::Ordinary||operation_id==0))return 0;
  if(operation_id==0)operation_id=doctor.begin_operation();
  if(operation_id==0)return 0;
  boshiamy::doctor::EventRecord record;
  record.operation_id=operation_id;
  record.monotonic_ns=boshiamy::doctor::monotonic_nanoseconds();
  record.event_code=static_cast<std::uint32_t>(event);
  record.callback_source=static_cast<std::uint32_t>(event);
  record.scope=static_cast<std::uint32_t>(scope);
  record.result_code=static_cast<std::int32_t>(FAILED(result)?result:session);
  record.duration_us=duration_us;
  record.flags=(has_focus?1u:0u)|(engine?2u:0u)|(modes.english()?4u:0u)|(doctor_composing?8u:0u)|(eaten?16u:0u);
  return doctor.try_write(record)==boshiamy::doctor::WriteResult::Written?operation_id:0;
 }
 std::uint64_t trace_operation(boshiamy::doctor::Event event,ITfContext* context,HRESULT result,HRESULT session,std::uint64_t operation_id,BOOL eaten=FALSE,std::uint32_t duration_us=0) noexcept {
  return trace(event,context,result,session,eaten,nullptr,operation_id,duration_us);
 }
 HRESULT request_doctor_edit(ITfContext* context,ITfEditSession* edit,DWORD flags,HRESULT* session) {
  const auto operation=doctor_operation_id;
  const auto started=boshiamy::doctor::monotonic_nanoseconds();
  const HRESULT hr=context->RequestEditSession(client,edit,flags,session);
  trace_operation(boshiamy::doctor::Event::EditRequest,context,hr,session?*session:E_POINTER,
   operation,SUCCEEDED(hr)&&session&&SUCCEEDED(*session),doctor_elapsed_us(started));
  return hr;
 }
 std::atomic<ULONG> refs{1}; ITfThreadMgr* mgr=nullptr; ITfKeystrokeMgr* keys=nullptr; TfClientId client=0; bool advised=false;
 boshiamy::preserved_keys::PreservedKeys preserved_keys;
 boshiamy::preserved_keys::OperationResult preserved_registration{E_UNEXPECTED,S_OK};
 HRESULT preserved_cleanup_status=S_FALSE;
 boshiamy::ShiftToggleTracker shift_toggle;
 bool shift_uppercase=false,shift_lower_symbols=false;
 std::filesystem::path settings_path;
 std::optional<boshiamy::Settings> applied_settings;
 std::string settings_error;
 bool initial_settings_applied=false;
 HRESULT inherited_open_status=S_FALSE;
 boshiamy::OpenCloseBinding open_close;
 HRESULT open_close_status=E_UNEXPECTED,open_close_cleanup_status=S_FALSE;
 boshiamy::OpenCloseBinding conversion;
 HRESULT conversion_status=E_UNEXPECTED,conversion_cleanup_status=S_FALSE,shape_inheritance_status=S_FALSE;
 bool publishing_open=false,publishing_conversion=false,handling_open_change=false;
 struct FlagScope {bool& flag;explicit FlagScope(bool& value):flag(value){flag=true;}~FlagScope(){flag=false;}};
 struct ServicePin {Service* owner;explicit ServicePin(Service* value):owner(value){owner->AddRef();}~ServicePin(){owner->Release();}};
 std::optional<bool> pending_open;
 void apply_pending_open(){
  if(!pending_open||!engine||!engine->snapshot().composition.empty())return;
  bool desired=*pending_open;pending_open.reset();reset_shift_gesture();
  if(modes.english()==desired){dismiss_learning_hint();modes.toggle_english();}
  if(language_bar)language_bar->update_display(engine->snapshot().mode,modes);
  publish_open();publish_fullshape();
 }
 void open_close_changed(){
  if(publishing_open||handling_open_change||!mgr||!engine)return;
  ServicePin pin(this);
  FlagScope notification(handling_open_change);
  bool open=false;open_close_status=open_close.read(open);
  if(open_close_status==S_OK){pending_open=open;apply_pending_open();}
 }
 void publish_open(){
  if(publishing_open||pending_open||!open_close.subscribed())return;
  FlagScope publishing(publishing_open);open_close_status=open_close.write(!modes.english());
 }
 void conversion_changed(){
  if(publishing_conversion||handling_open_change||!mgr||!engine)return;
  ServicePin pin(this);FlagScope notification(handling_open_change);
  LONG flags=0;conversion_status=conversion.read_long(flags);
  if(conversion_status==S_OK){
   bool fullwidth=(static_cast<DWORD>(flags)&boshiamy::kFullshapeMask)!=0;
   if(modes.fullwidth()!=fullwidth)modes.toggle_fullwidth();
   pending_open=(static_cast<DWORD>(flags)&boshiamy::kNativeMask)!=0;
   apply_pending_open();
   if(language_bar)language_bar->update_display(engine->snapshot().mode,modes);
  }
 }
 void publish_fullshape(){
  if(publishing_conversion||!conversion.subscribed())return;
  FlagScope publishing(publishing_conversion);
  DWORD mask=boshiamy::kFullshapeMask;
  DWORD bits=modes.fullwidth()?boshiamy::kFullshapeMask:0;
  // Keep a host's requested language bit while an active composition defers it.
  if(!pending_open){mask|=boshiamy::kNativeMask;if(!modes.english())bits|=boshiamy::kNativeMask;}
  conversion_status=conversion.update_bits(mask,bits);
 }
 void attach_open_close(){
  open_close_status=open_close.attach(mgr,client,[this](){open_close_changed();},std::make_shared<ModuleObjectLifetime>());
  if(SUCCEEDED(open_close_status))publish_open();
  conversion_status=conversion.attach(mgr,client,[this](){conversion_changed();},std::make_shared<ModuleObjectLifetime>(),boshiamy::kConversionCompartment);
  if(SUCCEEDED(conversion_status))publish_fullshape();
 }
 std::optional<bool> inherited_fullshape(){
  ITfCompartmentMgr* cm=nullptr;ITfCompartment* value=nullptr;std::optional<bool> result;
  shape_inheritance_status=mgr->QueryInterface(IID_ITfCompartmentMgr,reinterpret_cast<void**>(&cm));
  if(SUCCEEDED(shape_inheritance_status))shape_inheritance_status=cm->GetCompartment(boshiamy::kConversionCompartment,&value);
  if(cm)cm->Release();
  if(value){VARIANT flags;VariantInit(&flags);shape_inheritance_status=value->GetValue(&flags);if(shape_inheritance_status==S_OK){if(flags.vt==VT_I4)result=(static_cast<DWORD>(flags.lVal)&boshiamy::kFullshapeMask)!=0;else if(flags.vt==VT_EMPTY)shape_inheritance_status=S_FALSE;else shape_inheritance_status=DISP_E_TYPEMISMATCH;}VariantClear(&flags);value->Release();}
  return result;
 }
 std::optional<bool> inherited_open(){
  ITfCompartmentMgr* compartments=nullptr;ITfCompartment* compartment=nullptr;std::optional<bool> result;
  inherited_open_status=mgr->QueryInterface(IID_ITfCompartmentMgr,(void**)&compartments);
  if(SUCCEEDED(inherited_open_status))inherited_open_status=compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,&compartment);
  if(compartments)compartments->Release();
  if(compartment){VARIANT value;VariantInit(&value);inherited_open_status=compartment->GetValue(&value);if(SUCCEEDED(inherited_open_status)){if(value.vt==VT_I4)result=value.lVal!=0;else if(value.vt==VT_EMPTY)inherited_open_status=S_FALSE;else inherited_open_status=DISP_E_TYPEMISMATCH;}VariantClear(&value);compartment->Release();}
  return result;
 }
 void reload_settings(bool initial=false){
  if(settings_path.empty()||!engine)return;
  try{
   auto next=boshiamy::Settings::load(settings_path);bool first_profile=initial&&!initial_settings_applied;
   if(next.reverse_mode==boshiamy::ReverseMode::Associated)next.reverse_mode=boshiamy::ReverseMode::Recommended;
   next.related_enter_space=false;
   if(!applied_settings||*applied_settings!=next){
    if(!applied_settings||applied_settings->shift_toggle!=next.shift_toggle)set_shift_toggle_enabled(next.shift_toggle);
    shift_uppercase=next.shift_uppercase;shift_lower_symbols=next.shift_lower_symbols;engine->set_show_extensions(next.show_extensions);engine->set_speed_hint_only(next.speed_hint_only);engine->set_reverse_mode(next.reverse_mode);engine->set_related_enter_space(next.related_enter_space);applied_settings=next;
   }
   if(first_profile){
    if(next.inherit_system){auto open=inherited_open();if(open&&modes.english()==*open)modes.toggle_english();auto shape=inherited_fullshape();if(shape&&modes.fullwidth()!=*shape)modes.toggle_fullwidth();}
    else {if(modes.english()!=next.default_english)modes.toggle_english();engine->set_mode(next.default_language);}
    initial_settings_applied=true;
   }
   settings_error.clear();sync_language_bar();
  }catch(const std::exception& error){settings_error=error.what();}
 }
 ITfContext* shift_context=nullptr;
 ITfContext* query_release_context=nullptr;unsigned query_release_keys=0;
 static unsigned query_release_bit(WPARAM key){return key==VK_RETURN?1u:key==VK_LEFT?2u:key==VK_PRIOR?4u:key==VK_NEXT?8u:0u;}
 void clear_query_releases(){query_release_keys=0;if(query_release_context){query_release_context->Release();query_release_context=nullptr;}}
 void forget_query_release(WPARAM key){query_release_keys&=~query_release_bit(key);if(!query_release_keys)clear_query_releases();}
 void remember_query_release(ITfContext* ctx,WPARAM key){
  if(!same_context(ctx,query_release_context))clear_query_releases();
  if(!query_release_context){query_release_context=ctx;ctx->AddRef();}
  query_release_keys|=query_release_bit(key);
 }
 bool owns_query_release(ITfContext* ctx,WPARAM key)const{return (query_release_keys&query_release_bit(key))&&same_context(ctx,query_release_context);}
 void reset_shift_gesture(){shift_toggle.reset();if(shift_context){shift_context->Release();shift_context=nullptr;}}
 static unsigned shift_key(WPARAM key,LPARAM flags){if(key==VK_SHIFT){unsigned scan=(static_cast<ULONG_PTR>(flags)>>16)&0xff;if(scan==0x2a)return VK_LSHIFT;if(scan==0x36)return VK_RSHIFT;}return static_cast<unsigned>(key);}
 static bool is_shift(WPARAM key){return key==VK_SHIFT||key==VK_LSHIFT||key==VK_RSHIFT;}
 void observe_shift_down(ITfContext* ctx,WPARAM key,LPARAM flags,bool control,bool alt,bool composing){
  if(!has_focus||!shift_toggle.enabled()){reset_shift_gesture();return;}
  if(shift_context&&!same_context(ctx,shift_context))reset_shift_gesture();
  const unsigned observed=shift_key(key,flags);
  // Use the state attached to this queued key message. The physical state
  // may already reflect a later press when fast taps are processed.
  bool other_shift_down=false;
  if(observed==VK_LSHIFT)other_shift_down=(GetKeyState(VK_RSHIFT)&0x8000)!=0;
  else if(observed==VK_RSHIFT)other_shift_down=(GetKeyState(VK_LSHIFT)&0x8000)!=0;
  else if(observed==VK_SHIFT)other_shift_down=(GetKeyState(VK_LSHIFT)&0x8000)!=0&&(GetKeyState(VK_RSHIFT)&0x8000)!=0;
  shift_toggle.key_down(observed,control,alt,composing,(static_cast<ULONG_PTR>(flags)&(1ull<<30))!=0,other_shift_down);
  if(shift_toggle.pending()&&!shift_context){shift_context=ctx;ctx->AddRef();}
 }
 bool has_focus=true,layout_refresh_pending=false,layout_subscription_active=false;
 std::size_t layout_generation=0;
 ITfContext* candidate_context=nullptr;ITfSource* layout_source=nullptr;DWORD layout_cookie=TF_INVALID_COOKIE;bool related_focus_cleanup_pending=false;
 HRESULT layout_watch_status=E_UNEXPECTED,layout_cleanup_status=S_OK;
 static bool same_context(ITfContext* a,ITfContext* b){if(!a||!b)return false;IUnknown *x=nullptr,*y=nullptr;HRESULT hx=a->QueryInterface(IID_IUnknown,(void**)&x),hy=b->QueryInterface(IID_IUnknown,(void**)&y);bool same=SUCCEEDED(hx)&&SUCCEEDED(hy)&&x==y;if(x)x->Release();if(y)y->Release();return same;}
 // Some TSF hosts omit WM_KEYDOWN's previous-state bit on repeats. Keep the
 // engine's confirmation key pending until this context reports its release.
 bool owns_phonetic_confirmation_key(ITfContext* ctx,WPARAM key) const {
  if(!engine||!candidate_context)return false;
  const char confirming_key=engine->phonetic_confirmation_key();
  return confirming_key!=0&&key==static_cast<WPARAM>(confirming_key)&&candidate_context&&same_context(ctx,candidate_context);
 }
 bool suppress_phonetic_confirmation_repeat(ITfContext* ctx,WPARAM key,bool shift,bool control,bool alt) const {
  return !shift&&!control&&!alt&&has_focus&&owns_phonetic_confirmation_key(ctx,key);
 }
 HRESULT stop_watching_context(){
  ++layout_generation;layout_refresh_pending=false;layout_subscription_active=false;
  if(layout_source&&layout_cookie!=TF_INVALID_COOKIE){
   HRESULT h=layout_source->UnadviseSink(layout_cookie);
   if(FAILED(h)&&h!=CONNECT_E_NOCONNECTION){layout_cleanup_status=h;return h;}
  }
  auto* source=layout_source;auto* context=candidate_context;
  layout_source=nullptr;layout_cookie=TF_INVALID_COOKIE;candidate_context=nullptr;layout_watch_status=E_UNEXPECTED;
  if(source)source->Release();
  if(context)context->Release();
  layout_cleanup_status=S_OK;return S_OK;
 }
 void watch_context(ITfContext* ctx){
  if(layout_subscription_active&&same_context(ctx,candidate_context))return;
   if(candidate_context&&!same_context(ctx,candidate_context)&&engine)engine->clear_phonetic_confirmation_key();
  if(FAILED(stop_watching_context()))return;
  candidate_context=ctx;candidate_context->AddRef();
  layout_watch_status=ctx->QueryInterface(IID_ITfSource,(void**)&layout_source);
  if(SUCCEEDED(layout_watch_status))layout_watch_status=layout_source->AdviseSink(boshiamy::layout_sink_iid,static_cast<ITfTextLayoutSink*>(this),&layout_cookie);
  if(FAILED(layout_watch_status)&&layout_source){layout_source->Release();layout_source=nullptr;layout_cookie=TF_INVALID_COOKIE;}
  layout_subscription_active=SUCCEEDED(layout_watch_status);
 }
 void request_candidate_refresh(){
  if(!mgr||!engine||!layout_subscription_active||(!composition&&engine->snapshot().hint.empty())||!candidate_context||!has_focus||layout_refresh_pending)return;
  std::size_t generation=layout_generation;auto* read=new(std::nothrow) LayoutRefresh(this,candidate_context,generation);if(!read)return;
  layout_refresh_pending=true;HRESULT session=E_FAIL;HRESULT h=candidate_context->RequestEditSession(client,read,TF_ES_ASYNC|TF_ES_READ,&session);read->Release();
  if((FAILED(h)||FAILED(session))&&generation==layout_generation){layout_refresh_pending=false;if(candidate_window)candidate_window->hide();}
 }
 HRESULT refresh_candidate(ITfContext* ctx,TfEditCookie ec,std::size_t generation){
  if(generation!=layout_generation)return S_FALSE;
  layout_refresh_pending=false;
  if(!mgr||!engine||!layout_subscription_active||(!composition&&engine->snapshot().hint.empty())||!has_focus||!same_context(ctx,candidate_context))return S_FALSE;
  update_candidate(ctx,ec,engine->snapshot());return S_OK;
 }
 std::function<HRESULT(const std::filesystem::path&)> launch_tool;
 std::function<HRESULT(const std::filesystem::path&,const std::wstring&)> launch_doctor_tool;
 boshiamy::KeyboardModes modes;
 std::unique_ptr<boshiamy::CandidateWindow> candidate_window;
 std::shared_ptr<boshiamy::candidate_ui::CandidateUiController> candidate_ui;
 std::shared_ptr<boshiamy::candidate_ui::Callbacks> candidate_ui_callbacks;
 std::vector<std::wstring> candidate_ui_last_strings;
 bool candidate_ui_last_valid=false;
 std::shared_ptr<boshiamy::candidate_ui::ReadingUiController> reading_ui;
 std::shared_ptr<boshiamy::candidate_ui::Callbacks> reading_ui_callbacks;
 struct CandidateTarget {Service* owner=nullptr;};
 std::shared_ptr<CandidateTarget> candidate_ui_target;
 std::shared_ptr<CandidateTarget> reading_ui_target;
 bool candidate_ui_publishing=false;
 // A Show request changes native visibility, not the host notification role
 // selected by BeginUIElement. Only its synchronous publication uses this.
 int candidate_ui_show_request=-1;
 struct CandidatePlacement {HWND owner=nullptr;RECT anchor{},workarea{};UINT dpi=96;bool valid=false;} candidate_placement;
 void hide_candidate(){if(candidate_window)candidate_window->hide();}
 void paint_candidate(){
  if(!has_focus||!engine||!candidate_placement.valid){hide_candidate();return;}
  const auto out=engine->snapshot();
  const bool hint=out.composition.empty()&&!out.hint.empty();
  const bool element_suppressed=hint?(reading_ui&&reading_ui->suppressed()):(candidate_ui&&candidate_ui->suppressed());
  const bool element_missing=hint?(!reading_ui||reading_ui->id()==0xffffffffu):(!candidate_ui||candidate_ui->id()==0xffffffffu);
  if((!hint&&(!composition||out.composition.empty()))||element_suppressed||(ui_elements_only_activation&&element_missing)){hide_candidate();return;}
  const auto place=candidate_placement;
  if(!place.owner||!IsWindow(place.owner)){hide_candidate();return;}
  if(!candidate_window||!candidate_window->hwnd()||candidate_window->owner()!=place.owner)candidate_window=std::make_unique<boshiamy::CandidateWindow>(place.owner);
  candidate_window->update(out,place.anchor,place.dpi,place.workarea);
 }
 HRESULT end_candidate_ui(){
  candidate_ui_last_strings.clear();candidate_ui_last_valid=false;
  if(candidate_ui_target)candidate_ui_target->owner=nullptr;
  candidate_ui_target.reset();candidate_ui_callbacks.reset();
  auto controller=candidate_ui;
  const bool publishing=candidate_ui_publishing;candidate_ui_publishing=true;
  const HRESULT ended=controller?controller->end():S_OK;
  candidate_ui_publishing=publishing;return ended;
 }
 HRESULT end_reading_ui(){
  if(reading_ui_target)reading_ui_target->owner=nullptr;
  reading_ui_target.reset();reading_ui_callbacks.reset();
  auto controller=reading_ui;
  const bool publishing=candidate_ui_publishing;candidate_ui_publishing=true;
  const HRESULT ended=controller?controller->end():S_OK;
  candidate_ui_publishing=publishing;return ended;
 }
 boshiamy::candidate_ui::Snapshot candidate_snapshot(ITfDocumentMgr* document,const boshiamy::Outcome& out){
  boshiamy::candidate_ui::Snapshot snapshot;snapshot.description=L"嘸．改";
  snapshot.guid={0x22fa1048,0xf1d3,0x465c,{0x92,0xc8,0xee,0xb1,0xd5,0x86,0x86,0xe1}};
  snapshot.document=document;snapshot.updated_flags=0x3e;
  if(out.layout_candidates)for(const auto& value:*out.layout_candidates)snapshot.strings.push_back(to_wide(boshiamy::candidate_display_text(value)));
  for(auto start:out.candidate_page_indices)snapshot.page_starts.push_back(static_cast<UINT>(start));
  // An active empty list still describes page zero. Keep the same host object
  // and its Show/Begin policy while the user edits an unmatched composition.
  if(snapshot.strings.empty())snapshot.page_starts={0};
  snapshot.current_page=static_cast<UINT>(out.page);
  snapshot.selection=out.candidate_selection<snapshot.strings.size()?static_cast<UINT>(out.candidate_selection):0;
  return snapshot;
 }
 bool behavior_context_ready() const {
  return mgr&&engine&&composition&&has_focus&&candidate_context&&candidate_ui_target&&
      candidate_ui_target->owner==this;
 }
 HRESULT behavior_set_selection(UINT index){
  if(!behavior_context_ready())return E_UNEXPECTED;
  if(!engine->set_candidate_selection_global(index))return E_INVALIDARG;
  if(!candidate_ui_publishing)publish_candidate(candidate_context);
  return S_OK;
 }
 HRESULT behavior_apply(boshiamy::Engine&& trial,const boshiamy::Outcome& out){
  if(!behavior_context_ready()||candidate_ui_publishing)return E_UNEXPECTED;
  begin_doctor_operation(candidate_context);doctor_composing=!out.composition.empty();
  auto* context=candidate_context;context->AddRef();
  auto* edit=new(std::nothrow) EditRequest(this,context,out,std::move(trial));
  if(!edit){context->Release();return E_OUTOFMEMORY;}
  HRESULT session=E_FAIL;
  HRESULT hr=request_doctor_edit(context,edit,TF_ES_SYNC|TF_ES_READWRITE,&session);
  edit->Release();context->Release();
  if(FAILED(hr)||FAILED(session))return FAILED(hr)?hr:session;
  if(mgr){publish_candidate(candidate_context);sync_language_bar();}
  clear_query_releases();
  return S_OK;
 }
 HRESULT behavior_finalize(UINT index){
  if(!behavior_context_ready()||candidate_ui_publishing)return E_UNEXPECTED;
  boshiamy::Engine trial=*engine;boshiamy::Outcome out;
  if(!trial.finalize_candidate_selection_global(index,out))return E_INVALIDARG;
  return behavior_apply(std::move(trial),out);
 }
 HRESULT behavior_abort(){
  if(!behavior_context_ready()||candidate_ui_publishing)return E_UNEXPECTED;
  if(engine->snapshot().related_stage)return E_INVALIDARG;
  boshiamy::Engine trial=*engine;auto out=trial.key(boshiamy::Key::Escape);
  return behavior_apply(std::move(trial),out);
 }
 std::wstring reading_hint_display(const boshiamy::Outcome& out){
  std::wstring value=to_wide(out.hint);for(auto& ch:value)if(ch>=L'a'&&ch<=L'z')ch-=L'a'-L'A';
  const wchar_t* label=out.hint_kind==boshiamy::HintKind::Recommended?L"建議碼：":out.hint_kind==boshiamy::HintKind::All?L"所有拆碼：":L"最簡碼：";
  return std::wstring(label)+value;
 }
 void publish_candidate(ITfContext* context) noexcept {
  try{publish_candidate_impl(context);}catch(...){hide_candidate();end_candidate_ui();end_reading_ui();}
 }
 void publish_candidate_impl(ITfContext* context){
  if(candidate_ui_publishing)return;
  ServicePin pin(this);FlagScope publishing(candidate_ui_publishing);
  const auto out=engine?engine->snapshot():boshiamy::Outcome{};
  const bool learning_hint=out.composition.empty()&&!out.hint.empty();
  if(!has_focus||(!learning_hint&&(!composition||out.composition.empty()))){
   end_candidate_ui();end_reading_ui();paint_candidate();return;
  }
  if(learning_hint){
   end_candidate_ui();
   if(!reading_ui||!context){if(ui_elements_only_activation)hide_candidate();else paint_candidate();return;}
   auto controller=reading_ui;
   boshiamy::candidate_ui::ReadingSnapshot snapshot{};snapshot.context=context;snapshot.text=reading_hint_display(out);snapshot.updated_flags=0x1f;
   if(!reading_ui_callbacks){
    reading_ui_target=std::make_shared<CandidateTarget>();reading_ui_target->owner=this;
    std::weak_ptr<CandidateTarget> target=reading_ui_target;
    reading_ui_callbacks=std::make_shared<boshiamy::candidate_ui::Callbacks>();
    reading_ui_callbacks->show=[target](BOOL show)->HRESULT{auto handle=target.lock();if(!handle||!handle->owner)return E_UNEXPECTED;ServicePin service(handle->owner);if(!show)service.owner->hide_candidate();if(!service.owner->candidate_ui_publishing)service.owner->publish_candidate(service.owner->candidate_context);return S_OK;};
    reading_ui_callbacks->is_shown=[target](BOOL* shown)->HRESULT{if(!shown)return E_POINTER;*shown=FALSE;auto handle=target.lock();if(!handle||!handle->owner)return S_FALSE;ServicePin service(handle->owner);*shown=service.owner->candidate_window&&service.owner->candidate_window->visible();return S_OK;};
   }
   auto callbacks=reading_ui_callbacks;
   HRESULT publication=controller->id()==0xffffffffu?controller->begin(snapshot,callbacks):controller->update(snapshot);
   if(reading_ui!=controller||!mgr||!has_focus)return;
   if(FAILED(publication)){end_reading_ui();if(ui_elements_only_activation)hide_candidate();else paint_candidate();return;}
   if(controller->id()!=0xffffffffu)paint_candidate();
   return;
  }
  end_reading_ui();
  auto controller=candidate_ui;
  if(!controller){if(ui_elements_only_activation)hide_candidate();else paint_candidate();return;}
  ITfDocumentMgr* document=nullptr;
  if(!context||FAILED(context->GetDocumentMgr(&document))||!document){if(document)document->Release();end_candidate_ui();hide_candidate();return;}
  auto release_document=[](ITfDocumentMgr* value){value->Release();};
  std::unique_ptr<ITfDocumentMgr,decltype(release_document)> document_scope(document,release_document);
  auto snapshot=candidate_snapshot(document,out);
  HRESULT publication=S_FALSE;
  if(!candidate_ui_callbacks){
   candidate_ui_target=std::make_shared<CandidateTarget>();candidate_ui_target->owner=this;
   std::weak_ptr<CandidateTarget> target=candidate_ui_target;
   candidate_ui_callbacks=std::make_shared<boshiamy::candidate_ui::Callbacks>();
   candidate_ui_callbacks->show=[target](BOOL show)->HRESULT{
    auto handle=target.lock();if(!handle||!handle->owner)return E_UNEXPECTED;
    ServicePin service(handle->owner);
    if(!show)service.owner->hide_candidate();
    if(!service.owner->candidate_ui_publishing){
     const int previous=service.owner->candidate_ui_show_request;
     service.owner->candidate_ui_show_request=show?1:0;
     service.owner->publish_candidate(service.owner->candidate_context);
     service.owner->candidate_ui_show_request=previous;
    }
    return S_OK;
   };
   candidate_ui_callbacks->is_shown=[target](BOOL* shown)->HRESULT{
    if(!shown)return E_POINTER;*shown=FALSE;
    auto handle=target.lock();if(!handle||!handle->owner)return S_FALSE;
    ServicePin service(handle->owner);
    *shown=service.owner->candidate_window&&service.owner->candidate_window->visible();return S_OK;
   };
   candidate_ui_callbacks->set_page_indices=[target](const std::vector<UINT>& starts)->HRESULT{
    auto handle=target.lock();if(!handle||!handle->owner)return E_UNEXPECTED;
    ServicePin service(handle->owner);auto* owner=service.owner;
    if(!owner->engine||!owner->composition||!owner->has_focus)return E_UNEXPECTED;
    std::vector<std::size_t> indices(starts.begin(),starts.end());
    if(!owner->engine->set_candidate_page_indices(indices))return E_INVALIDARG;
    // The UIElement setter writes its own page map after this callback.
    // Setter-only changes send no Update and do not resize the native popup;
    // the next accepted edit refreshes its contents and page selection.
    return S_OK;
   };
   candidate_ui_callbacks->set_selection=[target](UINT index)->HRESULT{
    auto handle=target.lock();if(!handle||!handle->owner)return E_UNEXPECTED;
    ServicePin service(handle->owner);return service.owner->behavior_set_selection(index);
   };
   candidate_ui_callbacks->finalize=[target](UINT index)->HRESULT{
    auto handle=target.lock();if(!handle||!handle->owner)return E_UNEXPECTED;
    ServicePin service(handle->owner);return service.owner->behavior_finalize(index);
   };
   candidate_ui_callbacks->abort=[target]()->HRESULT{
    auto handle=target.lock();if(!handle||!handle->owner)return E_UNEXPECTED;
    ServicePin service(handle->owner);return service.owner->behavior_abort();
   };
   auto empty=snapshot;
   empty.strings.clear();empty.page_starts.clear();empty.selection=0;empty.current_page=0;
   empty.updated_flags=0;
   publication=controller->begin(empty,candidate_ui_callbacks);
   if(SUCCEEDED(publication)&&candidate_ui==controller&&mgr&&has_focus&&candidate_ui_callbacks&&controller->id()!=0xffffffffu){
    if(controller->host_notifications()){
     empty.updated_flags=0x24;
     publication=controller->update(empty);
     if(SUCCEEDED(publication)&&candidate_ui==controller&&mgr&&has_focus&&candidate_ui_callbacks&&controller->id()!=0xffffffffu){
      auto accepted=candidate_snapshot(document,engine->snapshot());
      if(!accepted.strings.empty()){
       accepted.updated_flags=0x3e;
       publication=controller->update(accepted);
      }else{accepted.updated_flags=0x24;publication=controller->silent_replace(accepted);}
     }
    }else{
     auto accepted=candidate_snapshot(document,engine->snapshot());
     accepted.updated_flags=0;
     publication=controller->silent_replace(accepted);
    }
   }
  }else{
   if(candidate_ui_show_request==0){
    snapshot.updated_flags=0x24;
    publication=controller->update(snapshot);
   }else if(candidate_ui_show_request==1){
    publication=controller->silent_replace(snapshot,true);
   }else if(controller->host_notifications()){
    snapshot.updated_flags=candidate_ui_last_valid&&snapshot.strings==candidate_ui_last_strings?0x24:0x3e;
    publication=controller->update(snapshot);
   }else publication=controller->silent_replace(snapshot,true);
  }
  if(FAILED(publication)){end_candidate_ui();hide_candidate();return;}
  if(candidate_ui==controller&&mgr&&has_focus&&controller->id()!=0xffffffffu){
   auto accepted=candidate_snapshot(document,engine->snapshot());
   candidate_ui_last_strings=std::move(accepted.strings);candidate_ui_last_valid=true;
  }
  // A host sink can synchronously deactivate the TIP during Begin/Update.
  if(candidate_ui==controller&&mgr&&has_focus)paint_candidate();
 }
 struct MenuTarget {Service* owner=nullptr;};
 std::shared_ptr<MenuTarget> menu_target;
 boshiamy::LanguageBarItem* language_bar=nullptr;
 ITfLangBarItemMgr* language_bar_manager=nullptr;
 bool language_bar_added=false;
 HRESULT language_bar_status=E_UNEXPECTED;
 void sync_language_bar(){apply_pending_open();publish_open();publish_fullshape();if(language_bar&&engine)language_bar->update_display(engine->snapshot().mode,modes);}
 void detach_language_bar(){
  if(menu_target)menu_target->owner=nullptr;
  if(language_bar_manager){if(language_bar_added&&language_bar)language_bar_manager->RemoveItem(language_bar);language_bar_manager->Release();language_bar_manager=nullptr;}
  language_bar_added=false;
  if(language_bar){language_bar->Release();language_bar=nullptr;}
  menu_target.reset();language_bar_status=E_UNEXPECTED;
 }
 void attach_language_bar(){
  menu_target=std::make_shared<MenuTarget>();menu_target->owner=this;auto target=menu_target;
  language_bar=new boshiamy::LanguageBarItem(CLSID_BoshiamyTsf,[target](boshiamy::MenuCommand command)->HRESULT {return target->owner?target->owner->menu_command(command):E_UNEXPECTED;},boshiamy::IconSource::from_resource(dll_module(),101),std::make_shared<ModuleObjectLifetime>());
  try{
   language_bar->set_doctor_metadata(to_wide(boshiamy::build_info::product_version),boshiamy::doctor::doctor_directory().wstring());
   language_bar->set_doctor_status_reader([target](){return target->owner&&target->owner->doctor.can_write();});
  }
  catch(...){language_bar->set_doctor_metadata(to_wide(boshiamy::build_info::product_version),L"無法取得資料夾");}
  sync_language_bar();
  language_bar_status=mgr->QueryInterface(IID_ITfLangBarItemMgr,(void**)&language_bar_manager);
  if(SUCCEEDED(language_bar_status)){language_bar_status=language_bar_manager->AddItem(language_bar);language_bar_added=SUCCEEDED(language_bar_status);}
 }
 std::unique_ptr<boshiamy::Engine> engine; ITfComposition* composition=nullptr; CompositionSink* compSink=nullptr;
 friend class EditRequest; friend class CompositionSink; friend class LayoutRefresh;
 HRESULT cancel_related_seed_on_focus_loss(){
  if(!engine)return S_FALSE;
  if(!engine->snapshot().related_stage){related_focus_cleanup_pending=false;return S_FALSE;}
  if(!candidate_context){related_focus_cleanup_pending=true;if(candidate_window)candidate_window->hide();return E_UNEXPECTED;}
  boshiamy::Engine trial=*engine;trial.cancel_related_on_focus_loss();const auto outcome=trial.snapshot();
  begin_doctor_operation(candidate_context);doctor_composing=!outcome.composition.empty();
  ITfContext* context=candidate_context;context->AddRef();auto* edit=new(std::nothrow)EditRequest(this,context,outcome,std::move(trial));if(!edit){context->Release();related_focus_cleanup_pending=true;if(candidate_window)candidate_window->hide();return E_OUTOFMEMORY;}
  HRESULT session=E_FAIL;HRESULT hr=request_doctor_edit(context,edit,TF_ES_SYNC|TF_ES_READWRITE,&session);edit->Release();
  context->Release();
  if(FAILED(hr)||FAILED(session)){related_focus_cleanup_pending=true;if(candidate_window)candidate_window->hide();return FAILED(hr)?hr:session;}
  related_focus_cleanup_pending=false;publish_candidate(nullptr);sync_language_bar();return S_OK;
 }
 void dismiss_learning_hint(bool stop_watch=true){
  if(engine&&!engine->snapshot().hint.empty()){engine->dismiss_hint();if(stop_watch&&!composition)stop_watching_context();if(candidate_window)candidate_window->hide();}
  end_reading_ui();
 }
 void clear_composition(ITfComposition* c){if(c && composition==c){ServicePin pin(this);composition->Release();composition=nullptr;stop_watching_context();candidate_placement.valid=false;hide_candidate();end_candidate_ui();end_reading_ui();}}
 void update_candidate(ITfContext* ctx,TfEditCookie ec,const boshiamy::Outcome& out,bool retry_layout=false,bool defer_publication=false) {
  candidate_placement.valid=false;
  const bool learning_hint=out.composition.empty()&&!out.hint.empty();
  if(!has_focus||(!learning_hint&&(out.composition.empty()||!composition))){hide_candidate();if(!defer_publication)publish_candidate(ctx);return;}
  ITfContextView* view=nullptr;ITfRange* range=nullptr;ITfRange* caret=nullptr;
  HRESULT h=ctx->GetActiveView(&view);
  if(SUCCEEDED(h)){
   if(learning_hint){TF_SELECTION selection{};ULONG fetched=0;h=ctx->GetSelection(ec,TF_DEFAULT_SELECTION,1,&selection,&fetched);range=selection.range;if(SUCCEEDED(h)&&(!fetched||!range))h=E_FAIL;}
   else h=composition->GetRange(&range);
  }
  if(SUCCEEDED(h))h=range->Clone(&caret);
  if(range)range->Release();
  if(SUCCEEDED(h))h=caret->Collapse(ec,learning_hint?TF_ANCHOR_END:TF_ANCHOR_START);
  RECT anchor{};BOOL clipped=FALSE;
  if(SUCCEEDED(h))h=view->GetTextExt(ec,caret,&anchor,&clipped);
  if(caret)caret->Release();
  HWND owner=nullptr;
  if(SUCCEEDED(h)&&(FAILED(view->GetWnd(&owner))||!owner))owner=GetFocus();
  if(view)view->Release();
  // A host can finish layout only after this write session ends. Queue one
  // read instead of leaving the popup hidden until another physical key.
  // The read path does not retry itself, so a persistently missing layout
  // cannot create an endless chain of edit sessions.
  if(h==TF_E_NOLAYOUT&&retry_layout)request_candidate_refresh();
  if(FAILED(h)||!owner||!IsWindow(owner)||anchor.bottom<=anchor.top||anchor.right<anchor.left){hide_candidate();if(!defer_publication)publish_candidate(ctx);return;}
  MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
  if(!GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor)){hide_candidate();if(!defer_publication)publish_candidate(ctx);return;}
  using DpiForWindow=UINT(WINAPI*)(HWND);auto dpiForWindow=reinterpret_cast<DpiForWindow>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
  UINT dpi=dpiForWindow?dpiForWindow(owner):96;
  candidate_placement={owner,anchor,monitor.rcWork,dpi?dpi:96,true};
  // Publish only after the accepted trial has become the engine state. A host
  // can call SetPageIndex synchronously from Begin/Update; committing a stale
  // trial after that callback would silently discard the host's page layout.
  if(!defer_publication)publish_candidate(ctx);
 }
 HRESULT write_composition(ITfContext* ctx,TfEditCookie ec,ITfRange* range,const std::wstring& text){
  if(text.size()>static_cast<std::size_t>(std::numeric_limits<LONG>::max()))return E_INVALIDARG;
  HRESULT hr=range->SetText(ec,0,text.data(),static_cast<LONG>(text.size()));if(FAILED(hr))return hr;
  ITfRange* caret=nullptr;hr=range->Clone(&caret);
  if(SUCCEEDED(hr))hr=caret->Collapse(ec,TF_ANCHOR_END);
  if(SUCCEEDED(hr)){TF_SELECTION selection{};selection.range=caret;selection.style.ase=TF_AE_NONE;selection.style.fInterimChar=FALSE;hr=ctx->SetSelection(ec,1,&selection);}
  if(caret)caret->Release();return hr;
 }
 HRESULT write_committed_prefix(ITfContext* ctx,TfEditCookie ec,const std::wstring& commit,const std::wstring& composing){
  if(!composition)return E_UNEXPECTED;
  const auto combined=commit+composing;
  if(combined.size()>static_cast<std::size_t>(std::numeric_limits<LONG>::max()))return E_INVALIDARG;
  ITfRange *range=nullptr,*original_start=nullptr,*next=nullptr,*caret=nullptr;ITfRangeBackup* backup=nullptr;
  TF_SELECTION before{};ULONG fetched=0;bool changed=false;std::wstring original;
  HRESULT h=composition->GetRange(&range);
  do{
   if(FAILED(h))break;
   h=range->Clone(&original_start);if(FAILED(h))break;
   h=original_start->Collapse(ec,TF_ANCHOR_START);if(FAILED(h))break;
   h=ctx->GetSelection(ec,TF_DEFAULT_SELECTION,1,&before,&fetched);if(FAILED(h)||!fetched){if(SUCCEEDED(h))h=E_FAIL;break;}
   h=ctx->CreateRangeBackup(ec,range,&backup);
   if(h==E_NOTIMPL){
    wchar_t buffer[512]{};ULONG length=0;h=range->GetText(ec,0,buffer,512,&length);
    if(SUCCEEDED(h)&&length>=512)h=E_INVALIDARG;
    if(SUCCEEDED(h))original.assign(buffer,length);
   }
   if(FAILED(h))break;
   h=range->SetText(ec,0,combined.data(),static_cast<LONG>(combined.size()));if(FAILED(h))break;changed=true;
   h=range->Clone(&next);if(FAILED(h))break;
   LONG shifted=0;h=next->ShiftStart(ec,static_cast<LONG>(commit.size()),&shifted,nullptr);
   if(SUCCEEDED(h)&&shifted!=static_cast<LONG>(commit.size()))h=E_FAIL;
   if(FAILED(h))break;
   h=composition->ShiftStart(ec,next);if(FAILED(h))break;
   h=next->Clone(&caret);if(FAILED(h))break;
   h=caret->Collapse(ec,TF_ANCHOR_END);if(FAILED(h))break;
   TF_SELECTION after{};after.range=caret;after.style.ase=TF_AE_NONE;after.style.fInterimChar=FALSE;
   h=ctx->SetSelection(ec,1,&after);
  }while(false);
  if(FAILED(h)&&changed){
   HRESULT restored=composition->ShiftStart(ec,original_start);
   HRESULT text=backup?backup->Restore(ec,range):range->SetText(ec,0,original.data(),static_cast<LONG>(original.size()));
   HRESULT selection=ctx->SetSelection(ec,1,&before);
   if(FAILED(restored))h=restored;else if(FAILED(text))h=text;else if(FAILED(selection))h=selection;
  }
  if(before.range)before.range->Release();if(backup)backup->Release();if(caret)caret->Release();if(next)next->Release();if(original_start)original_start->Release();if(range)range->Release();return h;
 }
 HRESULT apply_edit(ITfContext* ctx,TfEditCookie ec,const boshiamy::Outcome& out) {
  const std::wstring commit=to_wide(out.commit), composing=to_wide(boshiamy::composition_display(out.composition));
  if(commit.size()>static_cast<std::size_t>(std::numeric_limits<LONG>::max())||composing.size()>static_cast<std::size_t>(std::numeric_limits<LONG>::max()))return E_INVALIDARG;
  if(!commit.empty()&&!composing.empty())return write_committed_prefix(ctx,ec,commit,composing);
  if(!out.commit.empty()) {
   if(composition){ITfRange* r=nullptr;HRESULT h=composition->GetRange(&r);if(SUCCEEDED(h))h=r->SetText(ec,0,commit.data(),(LONG)commit.size());if(FAILED(h)){if(r)r->Release();return h;}ITfComposition* old=composition;h=old->EndComposition(ec);clear_composition(old);if(SUCCEEDED(h)){h=r->Collapse(ec,TF_ANCHOR_END);if(SUCCEEDED(h)){TF_SELECTION selection{};selection.range=r;selection.style.ase=TF_AE_NONE;selection.style.fInterimChar=FALSE;h=ctx->SetSelection(ec,1,&selection);}}r->Release();return h;}
   TF_SELECTION selection{};ULONG fetched=0;HRESULT h=ctx->GetSelection(ec,TF_DEFAULT_SELECTION,1,&selection,&fetched);if(FAILED(h)||!fetched)return FAILED(h)?h:E_FAIL;
   h=selection.range->SetText(ec,0,commit.data(),(LONG)commit.size());
   if(SUCCEEDED(h))h=selection.range->Collapse(ec,TF_ANCHOR_END);
   if(SUCCEEDED(h)){selection.style.ase=TF_AE_NONE;selection.style.fInterimChar=FALSE;h=ctx->SetSelection(ec,1,&selection);}
   selection.range->Release();return h;
  }
  if(composing.empty()) {
   if(!composition)return S_OK;ITfRange* r=nullptr;HRESULT h=composition->GetRange(&r);if(SUCCEEDED(h)){h=r->SetText(ec,0,L"",0);r->Release();}if(FAILED(h))return h;
   ITfComposition* old=composition;h=old->EndComposition(ec);if(SUCCEEDED(h))clear_composition(old);return h;
  }
  if(composition){ITfRange* r=nullptr;HRESULT h=composition->GetRange(&r);if(SUCCEEDED(h)){h=write_composition(ctx,ec,r,composing);r->Release();}return h;}
  TF_SELECTION selection{};ULONG fetched=0;HRESULT h=ctx->GetSelection(ec,TF_DEFAULT_SELECTION,1,&selection,&fetched);if(FAILED(h)||!fetched)return FAILED(h)?h:E_FAIL;
  ITfContextComposition* cc=nullptr;h=ctx->QueryInterface(IID_ITfContextComposition,(void**)&cc);if(FAILED(h)){selection.range->Release();return h;}
  h=cc->StartComposition(ec,selection.range,compSink,&composition);cc->Release();selection.range->Release();if(FAILED(h))return h;
  ITfRange* r=nullptr;h=composition->GetRange(&r);if(SUCCEEDED(h)){h=write_composition(ctx,ec,r,composing);r->Release();}return h;
 }
public:
 explicit Service(std::function<HRESULT(const std::filesystem::path&)> launcher=launch_tool_process,std::filesystem::path configuration={},boshiamy::AssociatedPhrases fixture_phrases={},DoctorScopeReader scope_reader={},std::function<HRESULT(const std::filesystem::path&,const std::wstring&)> doctor_launcher=launch_tool_process_arguments):doctor_scope_reader(std::move(scope_reader)),settings_path(std::move(configuration)),launch_tool(std::move(launcher)),launch_doctor_tool(std::move(doctor_launcher)),engine(load_engine()){doctor_scope_lifetime=std::make_shared<DoctorScopeLifetime>();doctor_scope_lifetime->owner=this;doctor_scope_pin=std::make_shared<ModuleObjectLifetime>();doctor.ensure();if(engine)engine->set_associated_phrases(std::move(fixture_phrases));shift_toggle.set_enabled(true);++objects;trace(boshiamy::doctor::Event::Created);} ~Service(){if(doctor_scope_lifetime)doctor_scope_lifetime->owner=nullptr;clear_doctor_scope();doctor_unadvise_thread_events();end_reading_ui();end_candidate_ui();reading_ui.reset();candidate_ui.reset();conversion.detach();open_close.detach();reset_shift_gesture();clear_query_releases();stop_watching_context();detach_language_bar();candidate_window.reset();if(composition){composition->Release();composition=nullptr;}if(compSink){compSink->Release();compSink=nullptr;}if(keys){preserved_keys.clear(*keys);keys->Release();}if(mgr)mgr->Release();--objects;}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,IID_ITfTextInputProcessor))*out=static_cast<ITfTextInputProcessor*>(this);else if(IsEqualIID(id,boshiamy::tsf_activation::kTextInputProcessorExIid)){*out=static_cast<ITfTextInputProcessorEx*>(this);trace(boshiamy::doctor::Event::ExtendedInterfaceQuery,nullptr,S_OK);}else if(IsEqualIID(id,IID_ITfKeyEventSink))*out=static_cast<ITfKeyEventSink*>(this);else if(IsEqualIID(id,IID_ITfThreadMgrEventSink))*out=static_cast<ITfThreadMgrEventSink*>(this);else if(IsEqualIID(id,IID_ITfTextEditSink))*out=static_cast<ITfTextEditSink*>(this);else if(IsEqualIID(id,boshiamy::layout_sink_iid))*out=static_cast<ITfTextLayoutSink*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;} ULONG STDMETHODCALLTYPE Release() override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* m,TfClientId c) override {
  return activate_from(m,c,1,0);
 }
 HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* m,TfClientId c,DWORD flags) override {
  return activate_from(m,c,2,flags);
 }
 HRESULT activate_from(ITfThreadMgr* m,TfClientId c,std::uint32_t entry,DWORD flags) {
  activation_entry_point=entry;activation_argument_flags=flags;ui_elements_only_activation=entry==2&&(flags&TF_TMAE_UIELEMENTENABLEDONLY)!=0;
  trace(boshiamy::doctor::Event::ActivateBegin,nullptr,S_OK,S_FALSE,FALSE,m);
  HRESULT hr=activate_impl(m,c);trace(boshiamy::doctor::Event::ActivateEnd,nullptr,hr);return hr;
 }
 HRESULT activate_impl(ITfThreadMgr* m,TfClientId c) {if(!m)return E_INVALIDARG;if(mgr==m){refresh_doctor_scope();return S_OK;}if(mgr||keys||layout_source||candidate_context||candidate_ui||reading_ui||open_close.subscribed()||conversion.subscribed()){HRESULT cleanup=Deactivate();if(FAILED(cleanup))return cleanup;}ui_elements_only_activation=activation_entry_point==2&&(activation_argument_flags&TF_TMAE_UIELEMENTENABLEDONLY)!=0;mgr=m;mgr->AddRef();client=c;has_focus=true;doctor_advise_thread_events();refresh_doctor_scope();reload_settings(true);
  compSink=new(std::nothrow) CompositionSink(this);if(!compSink){mgr->Release();mgr=nullptr;return E_OUTOFMEMORY;}
  // With no local dictionary, activation succeeds while every key remains available to the application.
  if(!engine)return S_OK;
  ITfUIElementMgr* ui_manager=nullptr;
  if(SUCCEEDED(m->QueryInterface(IID_ITfUIElementMgr,reinterpret_cast<void**>(&ui_manager)))&&ui_manager){
   try{candidate_ui=boshiamy::candidate_ui::CandidateUiController::Create(ui_manager,std::make_shared<ModuleObjectLifetime>());reading_ui=boshiamy::candidate_ui::ReadingUiController::Create(ui_manager,std::make_shared<ModuleObjectLifetime>());}
   catch(...){ui_manager->Release();Deactivate();return E_OUTOFMEMORY;}
   ui_manager->Release();
  }
  HRESULT hr=m->QueryInterface(IID_ITfKeystrokeMgr,(void**)&keys);if(SUCCEEDED(hr))hr=keys->AdviseKeyEventSink(c,this,TRUE);
  trace(boshiamy::doctor::Event::KeySinkAdvice,nullptr,hr);
  if(SUCCEEDED(hr)){advised=true;preserved_registration=preserved_keys.register_all(*keys,client);attach_open_close();attach_language_bar();return S_OK;} if(hr==E_INVALIDARG){preserved_registration={hr,S_OK};if(keys){keys->Release();keys=nullptr;}attach_open_close();attach_language_bar();return S_OK;}
  if(keys){keys->Release();keys=nullptr;}candidate_ui.reset();reading_ui.reset();compSink->Release();compSink=nullptr;mgr->Release();mgr=nullptr;return hr;
 }
 HRESULT STDMETHODCALLTYPE Deactivate() override {
  ServicePin pin(this);
  HRESULT result=deactivate_impl();trace(boshiamy::doctor::Event::Deactivate,nullptr,result);return result;
 }
 HRESULT deactivate_impl() {
  has_focus=false;clear_doctor_scope();doctor_unadvise_thread_events();if(engine)engine->clear_phonetic_confirmation_key();candidate_placement.valid=false;hide_candidate();HRESULT reading_cleanup=end_reading_ui();HRESULT ui_cleanup=end_candidate_ui();if(SUCCEEDED(ui_cleanup))candidate_ui.reset();if(SUCCEEDED(reading_cleanup))reading_ui.reset();clear_query_releases();dismiss_learning_hint(false);conversion_cleanup_status=conversion.detach();open_close_cleanup_status=open_close.detach();pending_open.reset();reset_shift_gesture();HRESULT layout_cleanup=stop_watching_context();detach_language_bar();candidate_window.reset();ui_elements_only_activation=false;HRESULT hr=FAILED(ui_cleanup)?ui_cleanup:FAILED(reading_cleanup)?reading_cleanup:FAILED(layout_cleanup)?layout_cleanup:FAILED(conversion_cleanup_status)?conversion_cleanup_status:FAILED(open_close_cleanup_status)?open_close_cleanup_status:S_OK;
  if(keys){preserved_cleanup_status=preserved_keys.clear(*keys).operation;if(FAILED(preserved_cleanup_status)&&SUCCEEDED(hr))hr=preserved_cleanup_status;
   if(advised){HRESULT unadvised=keys->UnadviseKeyEventSink(client);if(FAILED(unadvised)&&SUCCEEDED(hr))hr=unadvised;}advised=false;
   if(!preserved_keys.owns(boshiamy::preserved_keys::Key::language_toggle)&&!preserved_keys.owns(boshiamy::preserved_keys::Key::shape_toggle)){keys->Release();keys=nullptr;}
  }
  if(composition){composition->Release();composition=nullptr;}if(compSink){compSink->Release();compSink=nullptr;}if(mgr){mgr->Release();mgr=nullptr;}client=0;return hr;
 }
 boshiamy::preserved_keys::OperationResult preserved_key_result()const{return preserved_registration;}
 HRESULT preserved_key_cleanup_result()const{return preserved_cleanup_status;}
 boshiamy::LanguageBarItem* language_bar_item() const {return language_bar;}
 HRESULT language_bar_result() const {return language_bar_status;}
 const std::string& settings_load_error()const{return settings_error;}
 HRESULT inheritance_result()const{return inherited_open_status;}
 HRESULT open_close_result()const{return open_close_status;}
 HRESULT open_close_cleanup_result()const{return open_close_cleanup_status;}
 HRESULT layout_cleanup_result()const{return layout_cleanup_status;}
 HRESULT conversion_result()const{return conversion_status;}
 HRESULT shape_inheritance_result()const{return shape_inheritance_status;}
 HRESULT menu_command(boshiamy::MenuCommand command){
  if(!engine||!mgr)return E_UNEXPECTED;
  if(command==boshiamy::MenuCommand::DoctorSettings||command==boshiamy::MenuCommand::DoctorFolder)
   return launch_doctor_tool?launch_doctor_tool(dll_directory()/L"LiuKaiSettings.exe",command==boshiamy::MenuCommand::DoctorSettings?L"--doctor-settings":L"--doctor-folder"):E_UNEXPECTED;
  apply_pending_open();
  if(!engine->snapshot().composition.empty())return E_PENDING;
  dismiss_learning_hint();
  using boshiamy::MenuCommand;using boshiamy::Mode;
  switch(command){
  case MenuCommand::Settings:return launch_tool?launch_tool(dll_directory()/L"LiuKaiSettings.exe"):E_UNEXPECTED;
  case MenuCommand::Lookup:return launch_tool?launch_tool(dll_directory()/L"LiuKaiLookup.exe"):E_UNEXPECTED;
  case MenuCommand::Traditional:engine->set_mode(Mode::Traditional);break;
  case MenuCommand::Simplified:engine->set_mode(Mode::Simplified);break;
  case MenuCommand::TradToSimplified:engine->set_mode(Mode::TradToSimplified);break;
  case MenuCommand::Japanese:engine->set_mode(Mode::Japanese);break;
  case MenuCommand::Fast:engine->set_mode(Mode::Fast);break;
  case MenuCommand::English:if(!modes.english())modes.toggle_english();break;
  case MenuCommand::Chinese:if(modes.english())modes.toggle_english();break;
  case MenuCommand::Fullwidth:if(!modes.fullwidth())modes.toggle_fullwidth();break;
  case MenuCommand::Halfwidth:if(modes.fullwidth())modes.toggle_fullwidth();break;
  default:return E_INVALIDARG;
  }
  sync_language_bar();return S_OK;
 }
 HWND candidate_window_handle() const{return candidate_window?candidate_window->hwnd():nullptr;}
 HRESULT layout_watch_result() const{return layout_watch_status;}
 HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr*) override{return S_OK;}
 HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* document) override{
  auto* context=boshiamy::doctor::document_context(document);
  if(context){if(doctor_scope_cache.is_context(context))clear_doctor_scope();context->Release();}
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* focus,ITfDocumentMgr*) override{
  auto* context=boshiamy::doctor::document_context(focus);
  if(context)doctor_attach_context(context);else clear_doctor_scope();
  if(context)context->Release();
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override{
  if(!context)return E_POINTER;
  auto* focused=boshiamy::doctor::focused_context(mgr);
  if(focused&&same_context(focused,context))doctor_attach_context(context);
  if(focused)focused->Release();
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override{
  if(context&&doctor_scope_cache.is_context(context))refresh_doctor_scope();
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context,TfEditCookie cookie,ITfEditRecord* record) override{
  if(!context)return E_POINTER;
  const bool invalid=doctor_scope_cache.needs_refresh(context);
  if(!doctor_scope_cache.is_context(context))doctor_attach_context(context,false);
  if(invalid||boshiamy::doctor::input_scope_property_changed(record)){
   const auto generation=doctor_scope_cache.invalidate(context);
   const auto observation=boshiamy::doctor::read_input_scope_with_cookie(context,cookie);
   doctor_scope_completed(context,generation,observation);
  }
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL focused) override{ServicePin pin(this);has_focus=focused!=FALSE;if(!has_focus){clear_doctor_scope();if(engine)engine->clear_phonetic_confirmation_key();clear_query_releases();dismiss_learning_hint();reset_shift_gesture();++layout_generation;layout_refresh_pending=false;cancel_related_seed_on_focus_loss();end_candidate_ui();if(candidate_window)candidate_window->on_focus_lost();}else{refresh_doctor_scope();if(related_focus_cleanup_pending)cancel_related_seed_on_focus_loss();if(engine&&engine->snapshot().composition.empty())reload_settings();if(related_focus_cleanup_pending){if(candidate_window)candidate_window->hide();}else request_candidate_refresh();}trace(boshiamy::doctor::Event::Focus);return S_OK;}
  HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext* ctx,TfLayoutCode code,ITfContextView*) override{if(!layout_subscription_active||!same_context(ctx,candidate_context))return S_OK;if(code==TF_LC_DESTROY){ServicePin pin(this);if(engine)engine->clear_phonetic_confirmation_key();if(!composition&&engine)engine->dismiss_hint();stop_watching_context();candidate_placement.valid=false;hide_candidate();end_reading_ui();end_candidate_ui();return S_OK;}request_candidate_refresh();return S_OK;}
 static bool root_key(WPARAM w,char& ch){ if(w>=0x41&&w<=0x5a){ch=(char)('a'+(w-0x41));return true;} switch(w){case VK_OEM_COMMA:ch=',';return true;case VK_OEM_PERIOD:ch='.';return true;case VK_OEM_7:ch=static_cast<char>(39);return true;case VK_OEM_4:ch='[';return true;case VK_OEM_6:ch=']';return true;default:return false;} }
 static bool lower_symbol_key(WPARAM w){return w==VK_OEM_COMMA||w==VK_OEM_PERIOD||w==VK_OEM_7||w==VK_OEM_4||w==VK_OEM_6;}
 static bool auxiliary_key(WPARAM w,char& ch){switch(w){case VK_OEM_1:ch=';';return true;case VK_OEM_2:ch='/';return true;case VK_OEM_MINUS:ch='-';return true;default:return false;}}
 static bool has_shortcut_modifier(bool control,bool alt){return control||alt;}
 static bool should_handle_key(WPARAM w,bool shift,bool control,bool alt,bool composing,bool hasCandidates){
  if(has_shortcut_modifier(control,alt))return false;
  char root=0,aux=0;bool mapped=root_key(w,root),extra=auxiliary_key(w,aux);
  return (extra&&composing&&!shift)||(mapped&&!(shift&&(root==','||root=='.')))||(shift&&(root==','||root=='.')&&hasCandidates)||
   (w==VK_SPACE&&composing)||(w==VK_RETURN&&composing)||(w==VK_BACK&&composing)||(w==VK_ESCAPE&&composing)||
   ((w==VK_PRIOR||w==VK_NEXT)&&hasCandidates)||(w>=0x30&&w<=0x39&&composing);
 }
 bool handles_key(WPARAM w,bool shift,bool control,bool alt,bool composing,bool hasCandidates) const {
  if(alt)return false;
  if(w==VK_SPACE&&shift&&!control)return true;
  if(!composing&&w==VK_SPACE&&control&&!shift)return true;
  if(control)return false;
  if(modes.english())return modes.fullwidth()&&boshiamy::printable_ascii((int)w,shift,false).has_value();
  if(shift_lower_symbols&&shift&&lower_symbol_key(w))return true;
  if(engine&&!shift&&(w==VK_UP||w==VK_DOWN)){const auto state=engine->snapshot();return state.query==boshiamy::QueryStage::Homophones&&!state.candidates.empty();}
  if(engine){auto state=engine->snapshot();if(w==VK_RETURN&&(state.query_preview||state.query==boshiamy::QueryStage::PhoneticInput))return true;if(w==VK_LEFT&&!shift&&state.query_preview)return true;if((w==VK_PRIOR||w==VK_NEXT)&&composing&&state.query==boshiamy::QueryStage::None)return true;}
  return should_handle_key(w,shift,control,alt,composing,hasCandidates);
 }
void set_shift_uppercase(bool uppercase){shift_uppercase=uppercase;}
 void set_shift_toggle_enabled(bool enabled){reset_shift_gesture();shift_toggle.set_enabled(enabled);}
 HRESULT finish_query_preview(ITfContext* ctx,bool preserve){
  boshiamy::Engine trial=*engine;
  auto out=preserve?trial.commit_query_preview():trial.key(boshiamy::Key::Escape);
  doctor_composing=!out.composition.empty();
  auto* edit=new(std::nothrow) EditRequest(this,ctx,out,std::move(trial));if(!edit)return E_OUTOFMEMORY;
  HRESULT session=E_FAIL;HRESULT hr=request_doctor_edit(ctx,edit,TF_ES_SYNC|TF_ES_READWRITE,&session);edit->Release();
  if(FAILED(hr)||FAILED(session))return FAILED(hr)?hr:session;
  publish_candidate(ctx);clear_query_releases();sync_language_bar();return S_OK;
 }
 HRESULT test_key_down_for_state(ITfContext* ctx,WPARAM w,LPARAM flags,bool shift,bool control,bool alt,BOOL* eaten){
  ServicePin pin(this);
   const auto started=boshiamy::doctor::monotonic_nanoseconds();
   doctor_operation_id=0;doctor_pending_test_operation=0;doctor_pending_test_context=nullptr;
  HRESULT hr=test_key_down_impl(ctx,w,flags,shift,control,alt,eaten);
   trace_operation(boshiamy::doctor::Event::TestDown,ctx,hr,S_FALSE,doctor_operation_id,eaten?*eaten:FALSE,doctor_elapsed_us(started));
   if(SUCCEEDED(hr)&&doctor_operation_id){doctor_pending_test_operation=doctor_operation_id;doctor_pending_test_context=ctx;}
   return hr;
 }
 HRESULT test_key_down_impl(ITfContext* ctx,WPARAM w,LPARAM flags,bool shift,bool control,bool alt,BOOL* eaten){
  if(!eaten)return E_POINTER;*eaten=FALSE;
  if(!engine||!mgr||!ctx||candidate_ui_publishing)return S_OK;
  if(boshiamy::tsf_activation::classify_context(boshiamy::tsf_activation::read_context_gates(ctx))==boshiamy::tsf_activation::ContextDisposition::Blocked){doctor_observe_blocked_context(ctx);return S_OK;}
  begin_doctor_operation(ctx);
  apply_pending_open();
  if(engine->snapshot().composition.empty())reload_settings();
  if(suppress_phonetic_confirmation_repeat(ctx,w,shift,control,alt)){*eaten=TRUE;return S_OK;}
  auto state=engine->snapshot();observe_shift_down(ctx,w,flags,control,alt,!state.composition.empty());
  doctor_composing=!state.composition.empty();
  if(state.query_preview&&!control&&!alt&&(w==VK_TAB||(w>='0'&&w<='9')))return finish_query_preview(ctx,w==VK_TAB);
  *eaten=handles_key(w,shift,control,alt,!state.composition.empty(),!state.candidates.empty());
  if(!*eaten)dismiss_learning_hint();
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* ctx,WPARAM w,LPARAM flags,BOOL* eaten) override {
  return test_key_down_for_state(ctx,w,flags,(GetKeyState(VK_SHIFT)&0x8000)!=0,(GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_MENU)&0x8000)!=0,eaten);
 }
 HRESULT test_key_up_for_state(ITfContext* ctx,WPARAM w,LPARAM flags,bool control,bool alt,BOOL* eaten){
  if(!eaten)return E_POINTER;*eaten=FALSE;
  if(!engine||!mgr||!ctx||!has_focus||candidate_ui_publishing){clear_query_releases();reset_shift_gesture();return S_OK;}
  if(query_release_context&&!same_context(ctx,query_release_context))clear_query_releases();
  if(control||alt)forget_query_release(w);
  if(owns_query_release(ctx,w)){*eaten=TRUE;return S_OK;}
   if(owns_phonetic_confirmation_key(ctx,w)){*eaten=TRUE;return S_OK;}
  bool composing=!engine->snapshot().composition.empty();unsigned key=shift_key(w,flags);
  if(composing&&!control&&!alt&&(w==VK_SPACE||w==VK_BACK)){*eaten=TRUE;return S_OK;}
  if(same_context(ctx,shift_context)&&shift_toggle.would_toggle(key,control,alt,composing)){*eaten=TRUE;return S_OK;}
  // Unclaimed releases still update modifier tracking; they will not reach OnKeyUp.
  if(shift_context&&!same_context(ctx,shift_context))reset_shift_gesture();
  else {shift_toggle.key_up(key,control,alt,composing);if(!shift_toggle.pending())reset_shift_gesture();}
  return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* ctx,WPARAM w,LPARAM flags,BOOL* eaten) override {
   const auto started=boshiamy::doctor::monotonic_nanoseconds();
   if(is_shift(w)){doctor_pending_release_operation=0;doctor_pending_release_context=nullptr;begin_doctor_operation(ctx);}
  HRESULT hr=test_key_up_for_state(ctx,w,flags,(GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_MENU)&0x8000)!=0,eaten);
   if(is_shift(w))trace_operation(boshiamy::doctor::Event::ShiftTestUp,ctx,hr,S_FALSE,doctor_operation_id,eaten?*eaten:FALSE,doctor_elapsed_us(started));
   if(is_shift(w)&&SUCCEEDED(hr)&&doctor_operation_id){doctor_pending_release_operation=doctor_operation_id;doctor_pending_release_context=ctx;}
  return hr;
 }
HRESULT key_down_for_state(ITfContext* ctx,WPARAM w,bool shift,bool control,bool alt,BOOL* eaten,bool caps=false,LPARAM flags=0){
  ServicePin pin(this);
   const auto started=boshiamy::doctor::monotonic_nanoseconds();
   doctor_operation_id=0;
  HRESULT hr=key_down_impl(ctx,w,shift,control,alt,eaten,caps,flags);
   trace_operation(boshiamy::doctor::Event::KeyDown,ctx,hr,S_FALSE,doctor_operation_id,eaten?*eaten:FALSE,doctor_elapsed_us(started));
   doctor_pending_test_operation=0;doctor_pending_test_context=nullptr;return hr;
 }
HRESULT key_down_impl(ITfContext* ctx,WPARAM w,bool shift,bool control,bool alt,BOOL* eaten,bool caps,LPARAM flags){
  if(!eaten)return E_POINTER;
  *eaten=FALSE;
  if(!engine||!mgr||!ctx||candidate_ui_publishing)return S_OK;
  if(boshiamy::tsf_activation::classify_context(boshiamy::tsf_activation::read_context_gates(ctx))==boshiamy::tsf_activation::ContextDisposition::Blocked){doctor_observe_blocked_context(ctx);return S_OK;}
  continue_doctor_operation(ctx,doctor_pending_test_operation,doctor_pending_test_context);
  apply_pending_open();
  if(engine->snapshot().composition.empty())reload_settings();
  auto current=engine->snapshot();
  doctor_composing=!current.composition.empty();
  if(suppress_phonetic_confirmation_repeat(ctx,w,shift,control,alt)){*eaten=TRUE;return S_OK;}
  if(current.query_preview&&!control&&!alt&&(w==VK_TAB||(w>='0'&&w<='9')))return finish_query_preview(ctx,w==VK_TAB);
  dismiss_learning_hint();
  observe_shift_down(ctx,w,flags,control,alt,!current.composition.empty());
  bool composing=!current.composition.empty(),hasCandidates=!current.candidates.empty();
  if(!handles_key(w,shift,control,alt,composing,hasCandidates))return S_OK;
  if(w==VK_LEFT&&current.query_preview){remember_query_release(ctx,w);*eaten=TRUE;return S_OK;}
  if(!composing&&w==VK_SPACE&&control&&!shift&&!alt){modes.toggle_english();sync_language_bar();*eaten=TRUE;return S_OK;}
  if(w==VK_SPACE&&shift&&!control&&!alt){modes.toggle_fullwidth();sync_language_bar();*eaten=TRUE;return S_OK;}
  char root=0,aux=0;bool mapped=root_key(w,root),extra=auxiliary_key(w,aux);
  boshiamy::Engine trial=*engine;boshiamy::Outcome out;
  if(modes.english()){out=trial.snapshot();out.commit=boshiamy::fullwidth_utf8((int)w,shift,caps);}
  else if(shift&&w>='A'&&w<='Z'){
   out=trial.key(boshiamy::Key::Escape);
   // Shift requests literal English in Chinese mode. CapsLock still controls
   // case; the explicit uppercase preference inverts that base consistently.
   const char literal=*boshiamy::printable_ascii(static_cast<int>(w),shift_uppercase,caps);
   if(composing&&!engine->is_phonetic_input()){out.commit=current.composition;for(char& letter:out.commit)if(letter>='a'&&letter<='z')letter=static_cast<char>(letter-'a'+'A');out.commit.push_back(literal);}
   else if(modes.fullwidth())out.commit=boshiamy::fullwidth_utf8(static_cast<int>(w),shift_uppercase,caps);
   else out.commit.assign(1,literal);
  }
  else if(w==VK_SPACE)out=trial.key(boshiamy::Key::Space);
  else if(w==VK_RETURN)out=trial.key(boshiamy::Key::Enter);
  else if(w==VK_BACK)out=trial.key(boshiamy::Key::Backspace);
  else if(w==VK_ESCAPE)out=trial.key(boshiamy::Key::Escape);
  else if(w==VK_PRIOR)out=trial.key(boshiamy::Key::PageUp);
  else if(w==VK_NEXT)out=trial.key(boshiamy::Key::PageDown);
  else if(w==VK_UP)out=trial.key(boshiamy::Key::Up);
  else if(w==VK_DOWN)out=trial.key(boshiamy::Key::Down);
  else if(w>=0x30&&w<=0x39)out=trial.key((char)w,shift);
  else if(mapped)out=trial.key(root,shift,shift_lower_symbols);
  else if(extra)out=trial.key(aux);
  else return S_OK;
  auto* edit=new(std::nothrow) EditRequest(this,ctx,out,std::move(trial));
  if(!edit)return E_OUTOFMEMORY;
  HRESULT session=E_FAIL;
  HRESULT hr=request_doctor_edit(ctx,edit,TF_ES_SYNC|TF_ES_READWRITE,&session);
  edit->Release();
  if(FAILED(hr)||FAILED(session))return S_OK;
  if(!mgr){*eaten=TRUE;return S_OK;}
  publish_candidate(ctx);sync_language_bar();
  forget_query_release(w);if((w==VK_RETURN&&out.query_preview)||((w==VK_PRIOR||w==VK_NEXT||w==VK_UP||w==VK_DOWN)&&!out.composition.empty()))remember_query_release(ctx,w);
  *eaten=TRUE;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* ctx,WPARAM w,LPARAM flags,BOOL* eaten) override {
  const bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
  const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0;
  const bool alt=(GetKeyState(VK_MENU)&0x8000)!=0;
  return key_down_for_state(ctx,w,shift,control,alt,eaten,(GetKeyState(VK_CAPITAL)&1)!=0,flags);
}
 HRESULT key_up_for_state(ITfContext* ctx,WPARAM w,LPARAM flags,bool control,bool alt,BOOL* eaten){
  ServicePin pin(this);
  if(!eaten)return E_POINTER;*eaten=FALSE;
  if(!engine||!mgr||!ctx||!has_focus||candidate_ui_publishing){clear_query_releases();reset_shift_gesture();return S_OK;}
  if(query_release_context&&!same_context(ctx,query_release_context))clear_query_releases();
  if(control||alt)forget_query_release(w);
  if(owns_query_release(ctx,w)){forget_query_release(w);*eaten=TRUE;return S_OK;}
   if(owns_phonetic_confirmation_key(ctx,w)){
    engine->clear_phonetic_confirmation_key();*eaten=TRUE;return S_OK;
   }
  bool same=same_context(ctx,shift_context),composing=!engine->snapshot().composition.empty();
  if(composing&&!control&&!alt&&(w==VK_SPACE||w==VK_BACK)){*eaten=TRUE;return S_OK;}
  bool toggle=shift_toggle.key_up(shift_key(w,flags),control,alt,composing);
  if(!shift_toggle.pending())reset_shift_gesture();
  if(!same||!toggle)return S_OK;
  if(composing){
   boshiamy::Engine trial=*engine;auto out=trial.key(boshiamy::Key::Escape);
   doctor_composing=!out.composition.empty();
   auto* edit=new(std::nothrow)EditRequest(this,ctx,out,std::move(trial));if(!edit)return E_OUTOFMEMORY;
   HRESULT session=E_FAIL;HRESULT h=request_doctor_edit(ctx,edit,TF_ES_SYNC|TF_ES_READWRITE,&session);edit->Release();
   if(FAILED(h)||FAILED(session))return S_OK;
   publish_candidate(ctx);
  }
  pending_open.reset();modes.toggle_english();sync_language_bar();*eaten=TRUE;return S_OK;
 }
 HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* ctx,WPARAM w,LPARAM flags,BOOL* eaten) override {
   const auto started=boshiamy::doctor::monotonic_nanoseconds();
   if(is_shift(w))continue_doctor_operation(ctx,doctor_pending_release_operation,doctor_pending_release_context);
  HRESULT hr=key_up_for_state(ctx,w,flags,(GetKeyState(VK_CONTROL)&0x8000)!=0,(GetKeyState(VK_MENU)&0x8000)!=0,eaten);
   if(is_shift(w))trace_operation(boshiamy::doctor::Event::ShiftUp,ctx,hr,S_FALSE,doctor_operation_id,eaten?*eaten:FALSE,doctor_elapsed_us(started));
  return hr;
 }
 HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context,REFGUID command,BOOL* eaten) override {
  if(!eaten)return E_POINTER;*eaten=FALSE;if(!context||!has_focus)return S_OK;
  if(IsEqualGUID(command,boshiamy::preserved_keys::kLanguageToggleCommand))return key_down_for_state(context,VK_SPACE,false,true,false,eaten);
  if(IsEqualGUID(command,boshiamy::preserved_keys::kShapeToggleCommand))return key_down_for_state(context,VK_SPACE,true,false,false,eaten);
  return S_OK;
 }
};
CompositionSink::CompositionSink(Service* s):owner(s){owner->AddRef();}
CompositionSink::~CompositionSink(){owner->Release();}
HRESULT STDMETHODCALLTYPE CompositionSink::OnCompositionTerminated(TfEditCookie,ITfComposition* c){if(owner)owner->clear_composition(c);return S_OK;}
EditRequest::EditRequest(Service* s,ITfContext* c,const boshiamy::Outcome& o,boshiamy::Engine&& accepted):owner(s),context(c),outcome(o),accepted_engine(std::move(accepted)),doctor_operation_id(s->doctor_operation_id){owner->AddRef();context->AddRef();}
EditRequest::~EditRequest(){context->Release();owner->Release();}
HRESULT STDMETHODCALLTYPE EditRequest::QueryInterface(REFIID id,void** out){if(!out)return E_POINTER;*out=nullptr;if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,IID_ITfEditSession))*out=this;else return E_NOINTERFACE;AddRef();return S_OK;}
HRESULT EditRequest::DoEditSession(TfEditCookie ec){const auto started=boshiamy::doctor::monotonic_nanoseconds();HRESULT h=owner->apply_edit(context,ec,outcome);owner->doctor_composing=!outcome.composition.empty();owner->trace_operation(boshiamy::doctor::Event::EditApplied,context,h,S_OK,doctor_operation_id,SUCCEEDED(h),Service::doctor_elapsed_us(started));if(SUCCEEDED(h)){*owner->engine=std::move(accepted_engine);if(owner->mgr){if(!outcome.composition.empty()||!outcome.hint.empty())owner->watch_context(context);owner->update_candidate(context,ec,outcome,true,true);}}return h;}
LayoutRefresh::LayoutRefresh(Service* s,ITfContext* c,std::size_t g):owner(s),context(c),generation(g){owner->AddRef();context->AddRef();}
LayoutRefresh::~LayoutRefresh(){context->Release();owner->Release();}
HRESULT LayoutRefresh::DoEditSession(TfEditCookie ec){return owner->refresh_candidate(context,ec,generation);}
class Factory final: public IClassFactory {std::atomic<ULONG> refs{1};public:Factory(){++objects;}~Factory(){--objects;}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,IID_IClassFactory))*out=this;else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(outer)return CLASS_E_NOAGGREGATION;try{auto*s=new(std::nothrow)Service(launch_tool_process,boshiamy::settings_file_path());if(!s)return E_OUTOFMEMORY;HRESULT hr=s->QueryInterface(id,out);s->Release();return hr;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}}
 HRESULT STDMETHODCALLTYPE LockServer(BOOL lock)override{if(lock){++locks;return S_OK;}long n=locks.load();while(n>0){if(locks.compare_exchange_weak(n,n-1))return S_OK;}return E_UNEXPECTED;}};
extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID id,REFIID iid,void** out){if(!out)return E_POINTER;*out=nullptr;if(!IsEqualCLSID(id,CLSID_BoshiamyTsf))return CLASS_E_CLASSNOTAVAILABLE;auto*f=new(std::nothrow)Factory;if(!f)return E_OUTOFMEMORY;HRESULT h=f->QueryInterface(iid,out);f->Release();return h;}
extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(){return objects.load()==0&&locks.load()==0?S_OK:S_FALSE;}
extern "C" __declspec(dllexport) const CLSID* WINAPI BoshiamyTsf_GetClsid(){return &CLSID_BoshiamyTsf;}
extern "C" __declspec(dllexport) const GUID* WINAPI BoshiamyTsf_GetProfileGuid(){return &GUID_BoshiamyProfile;}
