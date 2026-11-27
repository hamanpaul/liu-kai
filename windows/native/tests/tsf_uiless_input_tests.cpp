#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "../tsf/service.cpp"
#include "../tsf/runtime_diagnostics.hpp"
#include "doctor_menu_fixture.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

class ContextRead final : public ITfEditSession {
    std::atomic<ULONG> refs{1}; ITfContext* context_;
public:
    std::wstring value;
    explicit ContextRead(ITfContext* context) : context_(context) { context_->AddRef(); }
    ~ContextRead() { context_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!IsEqualIID(iid,IID_IUnknown)&&!IsEqualIID(iid,IID_ITfEditSession))return E_NOINTERFACE;
        *out=static_cast<ITfEditSession*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        ITfRange *start=nullptr,*end=nullptr;
        HRESULT hr=context_->GetStart(cookie,&start);
        if(SUCCEEDED(hr))hr=context_->GetEnd(cookie,&end);
        if(SUCCEEDED(hr))hr=start->ShiftEndToRange(cookie,end,TF_ANCHOR_END);
        if(SUCCEEDED(hr)) {wchar_t buffer[64]{};ULONG count=0;hr=start->GetText(cookie,0,buffer,64,&count);if(SUCCEEDED(hr))value.assign(buffer,buffer+count);}
        if(end)end->Release();if(start)start->Release();return hr;
    }
};
static HRESULT read_context(ITfContext* context,TfClientId client,std::wstring* value) {
    auto* read=new ContextRead(context);HRESULT session=E_FAIL;
    const HRESULT hr=context->RequestEditSession(client,read,TF_ES_SYNC|TF_ES_READ,&session);
    if(SUCCEEDED(hr)&&SUCCEEDED(session))*value=read->value;
    read->Release();return FAILED(hr)?hr:session;
}
static bool write_fixture_file(const std::filesystem::path& path,const std::string& data) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;const BOOL wrote=WriteFile(file,data.data(),static_cast<DWORD>(data.size()),&written,nullptr);
    CloseHandle(file);return wrote&&written==data.size();
}
static HRESULT set_context_compartment(
    ITfContext* context,TfClientId client,REFGUID id,LONG value) {
    if(!context)return E_INVALIDARG;
    ITfCompartmentMgr* manager=nullptr;
    HRESULT hr=context->QueryInterface(
        IID_ITfCompartmentMgr,reinterpret_cast<void**>(&manager));
    if(FAILED(hr))return hr;
    ITfCompartment* compartment=nullptr;
    hr=manager->GetCompartment(id,&compartment);
    manager->Release();
    if(FAILED(hr))return hr;
    VARIANT data{};VariantInit(&data);
    data.vt=VT_I4;data.lVal=value;
    hr=compartment->SetValue(client,&data);
    VariantClear(&data);
    compartment->Release();
    return hr;
}
static boshiamy::candidate_ui::CandidateListUiElement* find_candidate_ui(ITfThreadMgr* manager) {
    ITfUIElementMgr* ui=nullptr;
    if(FAILED(manager->QueryInterface(IID_ITfUIElementMgr,reinterpret_cast<void**>(&ui)))||!ui)return nullptr;
    IEnumTfUIElements* elements=nullptr;boshiamy::candidate_ui::CandidateListUiElement* result=nullptr;
    if(SUCCEEDED(ui->EnumUIElements(&elements))&&elements){
        for(;;){ITfUIElement* element=nullptr;ULONG fetched=0;
            if(elements->Next(1,&element,&fetched)!=S_OK||fetched!=1)break;
            if(FAILED(element->QueryInterface(boshiamy::candidate_ui::iid_candidate_list_ui_element,
                reinterpret_cast<void**>(&result))))result=nullptr;
            element->Release();if(result)break;
        }
        elements->Release();
    }
    ui->Release();return result;
}
static std::wstring candidate_string(boshiamy::candidate_ui::CandidateListUiElement* element,UINT index) {
    BSTR value=nullptr;if(FAILED(element->GetString(index,&value))||!value)return {};
    std::wstring result(value,SysStringLen(value));SysFreeString(value);return result;
}
static bool candidate_page(boshiamy::candidate_ui::CandidateListUiElement* element,UINT* page) {
    return element&&page&&SUCCEEDED(element->GetCurrentPage(page));
}
static bool candidate_pages(boshiamy::candidate_ui::CandidateListUiElement* element,std::vector<UINT>* starts) {
    if(!element||!starts)return false;UINT count=0;
    if(FAILED(element->GetPageIndex(nullptr,0,&count))||count==0)return false;
    starts->assign(count,static_cast<UINT>(-1));return SUCCEEDED(element->GetPageIndex(starts->data(),count,&count));
}
static bool candidate_annotation(const std::wstring& value) {
    const auto space=value.find(L' ');return space!=std::wstring::npos&&space+1<value.size()&&
        value[space+1]>=L'A'&&value[space+1]<=L'Z';
}
static std::wstring candidate_text_only(const std::wstring& value) {
    const auto space=value.find(L' ');return value.substr(0,space);
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=2 && argc!=3)return 2;
    const auto directory=std::filesystem::absolute(argv[1]).lexically_normal();
    wchar_t executable[32768]{};const DWORD length=GetModuleFileNameW(nullptr,executable,32768);
    CHECK(length>0 && length<32768);
    const auto executableDirectory=std::filesystem::path(executable).parent_path().lexically_normal();
    CHECK(_wcsicmp(executableDirectory.c_str(),directory.c_str())==0);
    CHECK(directory.filename().wstring().find(L"tsf-uiless-fixture-")==0);
    CHECK(argc==2||argc==3);
    const auto local_app_data=directory/L"legacy-local-app-data";
    const auto legacy_directory=local_app_data/L"LiuKai";
    std::filesystem::create_directories(legacy_directory);
    CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA",local_app_data.c_str()));
    {
        std::ofstream marker(legacy_directory/boshiamy::runtime_diagnostics::kMarkerName,std::ios::binary);
        marker<<"LIUKAI_TSF_DIAGNOSTICS_V1\n"
              <<(boshiamy::runtime_diagnostics::detail::utc_now_seconds()+300)<<'\n';
        CHECK(marker.good());
    }
    const auto dictionary=directory/L"dictionary.tsv";
    const auto readings=directory/L"readings.tsv";
    struct PhoneticFixture {const char* text;const char* reading;};
    const PhoneticFixture phonetic_rows[]={
        {u8"蝦",u8"ㄒㄧㄚ"},{u8"瑕",u8"ㄒㄧㄚ"},{u8"匣",u8"ㄒㄧㄚ"},{u8"暇",u8"ㄒㄧㄚ"},
        {u8"瞎",u8"ㄒㄧㄚ"},{u8"遐",u8"ㄒㄧㄚ"},{u8"轄",u8"ㄒㄧㄚ"},{u8"俠",u8"ㄒㄧㄚ"},
        {u8"峽",u8"ㄒㄧㄚ"},{u8"呷",u8"ㄒㄧㄚ"},{u8"狎",u8"ㄒㄧㄚ"},{u8"瘕",u8"ㄒㄧㄚ"},
        {u8"霞",u8"ㄒㄧㄚˊ"},{u8"狹",u8"ㄒㄧㄚˇ"},{u8"下",u8"ㄒㄧㄚˋ"},{u8"夏",u8"˙ㄒㄧㄚ"},
        {u8"柙",u8"ㄒㄧㄚˇ"},{u8"祫",u8"ㄒㄧㄚˇ"},{u8"翈",u8"ㄒㄧㄚˇ"},
        {u8"甲",u8"ㄒㄧㄚˇ"},{u8"乙",u8"ㄒㄧㄚˇ"},{u8"丙",u8"ㄒㄧㄚˇ"},{u8"丁",u8"ㄒㄧㄚˇ"},
        {u8"戊",u8"ㄒㄧㄚˇ"},{u8"己",u8"ㄒㄧㄚˇ"},{u8"庚",u8"ㄒㄧㄚˇ"},{u8"辛",u8"ㄒㄧㄚˇ"}};
    std::string dictionary_data="section\tcode\ttext\tfreq\torder\n"
        "TRADITIONAL\ta\t對\t100\t0\nTRADITIONAL\toek\t嘸\t100\t1\n";
    std::string readings_data;
    for(std::size_t i=0;i<std::size(phonetic_rows);++i){
        const std::string code(1,static_cast<char>('b'+i%24));
        dictionary_data+="TRADITIONAL\t"+code+"\t"+phonetic_rows[i].text+"\t100\t"+std::to_string(i+2)+"\n";
        readings_data+=phonetic_rows[i].text;readings_data+='\t';readings_data+=phonetic_rows[i].reading;readings_data+='\n';
    }
    if(argc==3) {
        // Preserve the existing product-dictionary fixture mode; it remains
        // a focused test of ordinary Chinese commits.
        CHECK(CopyFileW(argv[2],dictionary.c_str(),TRUE));
    } else {
        CHECK(write_fixture_file(dictionary,dictionary_data));
        CHECK(write_fixture_file(readings,readings_data));
    }
    struct Cleanup {std::filesystem::path dictionary,readings;~Cleanup(){DeleteFileW(dictionary.c_str());DeleteFileW(readings.c_str());}} cleanup{dictionary,readings};
    CHECK(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)==S_OK);
    ITfThreadMgr* manager=nullptr;
    CHECK(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgr,
        reinterpret_cast<void**>(&manager))==S_OK);
    ITfThreadMgrEx* extended=nullptr;CHECK(manager->QueryInterface(IID_ITfThreadMgrEx,reinterpret_cast<void**>(&extended))==S_OK);
    TfClientId client=0;
    CHECK(extended->ActivateEx(&client,TF_TMAE_NOACTIVATETIP|TF_TMAE_UIELEMENTENABLEDONLY)==S_OK);
    extended->Release();
    ITfDocumentMgr* document=nullptr;ITfContext* context=nullptr;TfEditCookie cookie=0;
    CHECK(manager->CreateDocumentMgr(&document)==S_OK);
    CHECK(document->CreateContext(client,0,nullptr,&context,&cookie)==S_OK);
    CHECK(document->Push(context)==S_OK);
    CHECK(manager->SetFocus(document)==S_OK);
    boshiamy::doctor::ScopeObservation ordinary{};
    ordinary.context_available=true;
    ordinary.query_result=S_OK;
    ordinary.token_count=1;
    ordinary.tokens[0]=boshiamy::doctor::ScopeToken::Default;
    auto doctor_scope_state=std::make_shared<boshiamy::doctor::ScopeObservation>(ordinary);
    DoctorScopeReader doctor_scope_reader=[context,doctor_scope_state](
        ITfThreadMgr*,TfClientId,ITfContext** out)->boshiamy::doctor::ScopeObservation{
        if(out){*out=context;context->AddRef();}
        return *doctor_scope_state;
    };
    std::vector<std::pair<std::filesystem::path,std::wstring>> doctor_commands;
    auto* service=new Service(launch_tool_process,std::filesystem::path{},
        boshiamy::AssociatedPhrases{},std::move(doctor_scope_reader),
        [&](const std::filesystem::path& executable,const std::wstring& argument){
            doctor_commands.emplace_back(executable,argument);return S_OK;
        });
    CHECK(service->ActivateEx(manager,client,TF_TMAE_UIELEMENTENABLEDONLY)==S_OK);
    CHECK(service->language_bar_item());
    DoctorMenuCapture doctor_menu;
    CHECK(service->language_bar_item()->InitMenu(&doctor_menu)==S_OK);
    CHECK(doctor_menu.contains(to_wide(boshiamy::build_info::product_version)));
    CHECK(doctor_menu.contains((legacy_directory/L"doctor").wstring()));
    CHECK(doctor_menu.contains(L"Doctor 設定（未收集）"));
    CHECK(service->language_bar_item()->OnMenuSelect(static_cast<UINT>(boshiamy::MenuCommand::DoctorSettings))==S_OK);
    CHECK(service->language_bar_item()->OnMenuSelect(static_cast<UINT>(boshiamy::MenuCommand::DoctorFolder))==S_OK);
    CHECK(doctor_commands.size()==2);
    CHECK(doctor_commands[0].first==dll_directory()/L"LiuKaiSettings.exe");
    CHECK(doctor_commands[0].second==L"--doctor-settings");
    CHECK(doctor_commands[1].first==doctor_commands[0].first);
    CHECK(doctor_commands[1].second==L"--doctor-folder");
    auto pending_scope_pin=std::make_shared<ModuleObjectLifetime>();
    auto* pending_scope_session=new boshiamy::doctor::ScopeReadSession(
        context,1,nullptr,{},pending_scope_pin);
    CHECK(service->menu_command(boshiamy::MenuCommand::Chinese)==S_OK);
    CHECK(service->menu_command(boshiamy::MenuCommand::Traditional)==S_OK);
    std::uint64_t process_creation=0;
    CHECK(boshiamy::doctor::detail::process_creation_stamp(&process_creation));
    const auto doctor_name=boshiamy::doctor::detail::local_mapping_name(GetCurrentProcessId(),process_creation);
    HANDLE doctor_mapping=OpenFileMappingW(
        FILE_MAP_READ|FILE_MAP_WRITE|READ_CONTROL,FALSE,doctor_name.c_str());
    CHECK(doctor_mapping!=nullptr);
    void* doctor_view=MapViewOfFile(
        doctor_mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,
        boshiamy::doctor::SharedQueue<boshiamy::doctor::kQueueCapacity>::storage_bytes());
    CHECK(doctor_view!=nullptr);
    boshiamy::doctor::SharedQueue<boshiamy::doctor::kQueueCapacity> doctor_queue(doctor_view);
    CHECK(doctor_queue.initialize(0x5566778899aabbccULL,true));
    CHECK(doctor_queue.publish_scope(ordinary));
    DoctorMenuCapture collecting_menu;
    CHECK(service->language_bar_item()->InitMenu(&collecting_menu)==S_OK);
    CHECK(collecting_menu.contains(L"Doctor 設定（收集中）"));
    CHECK(set_context_compartment(
        context,client,GUID_COMPARTMENT_KEYBOARD_DISABLED,0)==S_OK);
    CHECK(set_context_compartment(
        context,client,GUID_COMPARTMENT_EMPTYCONTEXT,0)==S_OK);
    auto key_down=[&](WPARAM key,LPARAM flags=0) {
        BOOL tested=FALSE;HRESULT hr=service->OnTestKeyDown(context,key,flags,&tested);
        if(hr!=S_OK||!tested)return false;
        BOOL eaten=FALSE;hr=service->OnKeyDown(context,key,flags,&eaten);
        return hr==S_OK&&eaten;
    };
    auto key_up=[&](WPARAM key,LPARAM flags=0) {
        BOOL tested=FALSE;HRESULT hr=service->OnTestKeyUp(context,key,flags,&tested);
        if(hr!=S_OK||!tested)return false;
        BOOL eaten=FALSE;hr=service->OnKeyUp(context,key,flags,&eaten);
        return hr==S_OK&&eaten;
    };
    auto press=[&](WPARAM key) {const bool eaten=key_down(key);key_up(key);return eaten;};
    auto shifted_letter=[&](WPARAM key,bool uppercase,bool caps) {
        service->set_shift_uppercase(uppercase);BOOL tested=FALSE;
        HRESULT hr=service->test_key_down_for_state(context,key,0,true,false,false,&tested);
        if(hr!=S_OK||!tested)return false;
        BOOL eaten=FALSE;hr=service->key_down_for_state(context,key,true,false,false,&eaten,caps,0);
        return hr==S_OK&&eaten;
    };
    CHECK(press('A') && press(VK_SPACE));
    std::wstring text;CHECK(read_context(context,client,&text)==S_OK && text==L"\u5c0d");
    const auto ordinary_snapshot=doctor_queue.snapshot();
    CHECK(ordinary_snapshot.events_written>0);
    boshiamy::doctor::EventRecord observed{};
    std::size_t ordinary_events=0;
    std::uint64_t tested_operation=0;
    bool saw_key_down=false, saw_edit_applied=false;
    bool saw_measured_callback=false;
    bool saw_measured_request=false, saw_measured_apply=false;
    while(doctor_queue.try_read(&observed)==boshiamy::doctor::ReadResult::Read){
        CHECK(observed.scope==static_cast<std::uint32_t>(boshiamy::doctor::ScopeClass::Ordinary));
        const auto code=static_cast<boshiamy::doctor::Event>(observed.event_code);
        if(code==boshiamy::doctor::Event::TestDown){
            tested_operation=observed.operation_id;
            CHECK(tested_operation!=0);
        }else if(code==boshiamy::doctor::Event::KeyDown||
                 code==boshiamy::doctor::Event::EditRequest||
                 code==boshiamy::doctor::Event::EditApplied){
            CHECK(observed.operation_id==tested_operation);
            saw_key_down|=code==boshiamy::doctor::Event::KeyDown;
            saw_edit_applied|=code==boshiamy::doctor::Event::EditApplied;
            saw_measured_callback|=observed.duration_us!=0;
            if(code==boshiamy::doctor::Event::EditRequest) saw_measured_request|=observed.duration_us!=0;
            if(code==boshiamy::doctor::Event::EditApplied){
                saw_measured_apply|=observed.duration_us!=0;
                CHECK((observed.flags&16u)!=0);
            }
        }
        ++ordinary_events;
    }
    CHECK(ordinary_events>0);
    CHECK(saw_key_down&&saw_edit_applied);
    CHECK(saw_measured_callback);
    CHECK(saw_measured_request&&saw_measured_apply);
    BOOL shift_tested=FALSE, shift_eaten=FALSE;
    CHECK(service->OnTestKeyDown(context,VK_SHIFT,0x002a0001,&shift_tested)==S_OK);
    CHECK(service->OnTestKeyUp(context,VK_SHIFT,static_cast<LPARAM>(0xc02a0001),&shift_tested)==S_OK&&shift_tested);
    CHECK(service->OnKeyUp(context,VK_SHIFT,static_cast<LPARAM>(0xc02a0001),&shift_eaten)==S_OK&&shift_eaten);
    std::uint64_t tested_release=0;
    unsigned matched_releases=0;
    while(doctor_queue.try_read(&observed)==boshiamy::doctor::ReadResult::Read){
        const auto code=static_cast<boshiamy::doctor::Event>(observed.event_code);
        if(code==boshiamy::doctor::Event::ShiftTestUp) tested_release=observed.operation_id;
        if(code==boshiamy::doctor::Event::ShiftUp){
            CHECK(tested_release!=0&&observed.operation_id==tested_release);
            ++matched_releases;
        }
    }
    CHECK(matched_releases==1);
    struct QueueCounters {
        std::uint64_t operations=0;
        std::uint64_t writes=0;
        std::uint64_t events=0;
    };
    auto queue_counters=[&](){
        auto* storage=static_cast<
            boshiamy::doctor::SharedQueue<
                boshiamy::doctor::kQueueCapacity>::Storage*>(doctor_view);
        return QueueCounters{
            boshiamy::doctor::detail::read_i64(
                &storage->control.operation_index),
            boshiamy::doctor::detail::read_i64(
                &storage->control.write_index),
            boshiamy::doctor::detail::read_i64(
                &storage->control.events_written),
        };
    };
    const GUID blocked_compartments[]={
        GUID_COMPARTMENT_KEYBOARD_DISABLED,
        GUID_COMPARTMENT_EMPTYCONTEXT,
    };
    for(const auto& blocked_compartment:blocked_compartments){
        const auto blocked_before=queue_counters();
        CHECK(set_context_compartment(
            context,client,blocked_compartment,1)==S_OK);
        BOOL blocked_tested=FALSE,blocked_eaten=FALSE;
        CHECK(service->OnTestKeyUp(
            context,VK_SHIFT,static_cast<LPARAM>(0xc02a0001),
            &blocked_tested)==S_OK);
        CHECK(service->OnKeyUp(
            context,VK_SHIFT,static_cast<LPARAM>(0xc02a0001),
            &blocked_eaten)==S_OK);
        const auto blocked_after=queue_counters();
        CHECK(blocked_after.operations==blocked_before.operations);
        CHECK(blocked_after.writes==blocked_before.writes);
        CHECK(blocked_after.events==blocked_before.events);
        CHECK(doctor_queue.snapshot().scope_class==
            static_cast<std::uint32_t>(
                boshiamy::doctor::ScopeClass::Sensitive));

        CHECK(set_context_compartment(
            context,client,blocked_compartment,0)==S_OK);
        CHECK(doctor_queue.snapshot().scope_class==
            static_cast<std::uint32_t>(
                boshiamy::doctor::ScopeClass::Ordinary));
        const auto recovered_before=queue_counters();
        BOOL recovered_tested=FALSE,recovered_eaten=FALSE;
        CHECK(service->OnTestKeyUp(
            context,VK_SHIFT,static_cast<LPARAM>(0xc02a0001),
            &recovered_tested)==S_OK);
        CHECK(service->OnKeyUp(
            context,VK_SHIFT,static_cast<LPARAM>(0xc02a0001),
            &recovered_eaten)==S_OK);
        const auto recovered_after=queue_counters();
        CHECK(recovered_after.operations==
            recovered_before.operations+1);
        CHECK(recovered_after.writes==recovered_before.writes+2);
        CHECK(recovered_after.events==recovered_before.events+2);
        while(doctor_queue.try_read(&observed)==
            boshiamy::doctor::ReadResult::Read){}
    }
    CHECK(service->menu_command(boshiamy::MenuCommand::Chinese)==S_OK);
    auto sensitive=*doctor_scope_state;
    sensitive.tokens[0]=boshiamy::doctor::ScopeToken::Password;
    *doctor_scope_state=sensitive;
    CHECK(service->OnPushContext(context)==S_OK);
    CHECK(doctor_queue.snapshot().scope_class==
        static_cast<std::uint32_t>(boshiamy::doctor::ScopeClass::Sensitive));
    const auto sensitive_before=doctor_queue.snapshot().events_written;
    void* sensitive_ex=nullptr;
    CHECK(service->QueryInterface(boshiamy::tsf_activation::kTextInputProcessorExIid,&sensitive_ex)==S_OK);
    static_cast<ITfTextInputProcessorEx*>(sensitive_ex)->Release();
    CHECK(press('O')&&press('E')&&press('K')&&press(VK_SPACE));
    CHECK(read_context(context,client,&text)==S_OK && text==L"\u5c0d\u5638");
    CHECK(doctor_queue.snapshot().events_written==sensitive_before);

    boshiamy::doctor::ScopeObservation unknown{};
    unknown.context_available=true;
    unknown.query_result=static_cast<std::int32_t>(E_UNEXPECTED);
    *doctor_scope_state=unknown;
    CHECK(service->OnPushContext(context)==S_OK);
    CHECK(doctor_queue.snapshot().scope_class==
        static_cast<std::uint32_t>(boshiamy::doctor::ScopeClass::Unknown));
    const auto unknown_before=doctor_queue.snapshot().events_written;
    void* unknown_ex=nullptr;
    CHECK(service->QueryInterface(boshiamy::tsf_activation::kTextInputProcessorExIid,&unknown_ex)==S_OK);
    static_cast<ITfTextInputProcessorEx*>(unknown_ex)->Release();
    BOOL tested=FALSE;
    CHECK(service->OnTestKeyDown(context,'A',0,&tested)==S_OK);
    CHECK(tested);
    CHECK(doctor_queue.snapshot().events_written==unknown_before);

    *doctor_scope_state=ordinary;
    CHECK(service->OnPushContext(context)==S_OK);
    boshiamy::doctor::EventRecord full_record{};
    full_record.event_code=static_cast<std::uint32_t>(boshiamy::doctor::Event::Focus);
    full_record.scope=static_cast<std::uint32_t>(boshiamy::doctor::ScopeClass::Ordinary);
    for(std::size_t i=0;i<boshiamy::doctor::kQueueCapacity;++i)
        CHECK(doctor_queue.try_write(full_record)==boshiamy::doctor::WriteResult::Written);
    CHECK(doctor_queue.try_write(full_record)==boshiamy::doctor::WriteResult::Full);
    CHECK(doctor_queue.try_write(full_record)==boshiamy::doctor::WriteResult::Full);
    if(argc==3) {
        CHECK(doctor_queue.set_enabled(false));
        UnmapViewOfFile(doctor_view);CloseHandle(doctor_mapping);
        CHECK(service->Deactivate()==S_OK);service->Release();
        CHECK(document->Pop(TF_POPF_ALL)==S_OK);context->Release();document->Release();
        CHECK(manager->Deactivate()==S_OK);manager->Release();CoUninitialize();
        CHECK(DllCanUnloadNow()==S_FALSE);
        pending_scope_session->Release();
        pending_scope_pin.reset();
        CHECK(DllCanUnloadNow()==S_OK);
        for(const auto& entry:std::filesystem::directory_iterator(legacy_directory))
            CHECK(entry.path().filename().wstring().rfind(L"tsf-diagnostics-",0)!=0);
        std::cout << "UI-less TSF fixture committed two Chinese characters using supplied dictionary; no TIP was registered\n";
        return 0;
    }
    auto context_is=[&](const std::wstring& expected){std::wstring current;return read_context(context,client,&current)==S_OK&&current==expected;};
    std::wstring expected=L"\u5c0d\u5638";
    auto begin_zhuyin=[&](){return press(VK_OEM_7)&&press(VK_OEM_1);};
    CHECK(begin_zhuyin()&&key_down('V')&&key_down('V',static_cast<LPARAM>(0x40000000)));key_up('V');
    CHECK(context_is(expected+L"\u6ce8\u3112"));
    CHECK(press('U')&&press('8'));
    CHECK(context_is(expected+L"\u6ce8\u3112\u3127\u311a"));
    CHECK(press(VK_BACK)&&press('9'));
    CHECK(context_is(expected+L"\u6ce8\u3112\u3127\u311e"));
    CHECK(press(VK_ESCAPE));CHECK(context_is(expected));
    CHECK(begin_zhuyin()&&press('A')&&press('8'));
    CHECK(context_is(expected+L"\u6ce8\u3107\u311a"));
    CHECK(shifted_letter('A',false,false));expected+=L"a";CHECK(context_is(expected));
    const struct {WPARAM key;const wchar_t* glyph;} punctuation[]={
        {VK_OEM_1,L"\u3124"},{VK_OEM_COMMA,L"\u311d"},{VK_OEM_PERIOD,L"\u3121"},
        {VK_OEM_2,L"\u3125"},{VK_OEM_MINUS,L"\u3126"}};
    for(const auto& item:punctuation){CHECK(begin_zhuyin()&&press(item.key));CHECK(context_is(expected+std::wstring(L"\u6ce8")+item.glyph));CHECK(press(VK_ESCAPE));}
    const struct {WPARAM key;const wchar_t* tone;} tones[]={
        {'6',L"\u02ca"},{'3',L"\u02c7"},{'4',L"\u02cb"},{'7',L"\u02d9"}};
    for(const auto& tone:tones){CHECK(begin_zhuyin()&&press('V')&&press('U')&&press('8')&&press(tone.key));CHECK(context_is(expected+std::wstring(L"\u6ce8\u3112\u3127\u311a")+tone.tone));CHECK(press(VK_ESCAPE));}
    CHECK(begin_zhuyin()&&press('V')&&press('U')&&press('8'));
    CHECK(key_down('3'));
    auto* repeated_tone_candidates=find_candidate_ui(manager);CHECK(repeated_tone_candidates!=nullptr);
    UINT repeated_candidate_count=0;CHECK(repeated_tone_candidates->GetCount(&repeated_candidate_count)==S_OK&&repeated_candidate_count==12);
    const auto repeated_tone_candidate=candidate_text_only(candidate_string(repeated_tone_candidates,3));
    CHECK(!repeated_tone_candidate.empty());
    CHECK(key_down('3'));
    CHECK(context_is(expected+L"\u6ce8\u3112\u3127\u311a\u02c7"));
    CHECK(key_down('3',static_cast<LPARAM>(0x40000000)));
    CHECK(key_up('3'));
    CHECK(context_is(expected+L"\u6ce8\u3112\u3127\u311a\u02c7"));
    repeated_tone_candidates->Release();
    CHECK(press('3'));
    expected+=repeated_tone_candidate;CHECK(context_is(expected));
    CHECK(begin_zhuyin()&&press('V')&&press('U')&&press('8')&&press('3'));
    CHECK(press(VK_BACK));
    const LPARAM wpf_space_down=static_cast<LPARAM>(0x00390001);
    const LPARAM wpf_space_up=static_cast<LPARAM>(0xC0390001);
    CHECK(key_down(VK_SPACE,wpf_space_down));
    auto* retained_tone_candidates=find_candidate_ui(manager);CHECK(retained_tone_candidates!=nullptr);
    auto retained_page_is=[&](UINT wanted){
        retained_tone_candidates->Release();retained_tone_candidates=find_candidate_ui(manager);
        UINT current=99;return candidate_page(retained_tone_candidates,&current)&&current==wanted;
    };
    CHECK(retained_page_is(0));
    CHECK(key_down(VK_SPACE,wpf_space_down));CHECK(retained_page_is(0));
    CHECK(key_down(VK_SPACE,static_cast<LPARAM>(0x40000000)));CHECK(retained_page_is(0));
    CHECK(key_up(VK_SPACE,wpf_space_up));
    CHECK(key_down(VK_SPACE,wpf_space_down));CHECK(retained_page_is(1));
    CHECK(key_down(VK_SPACE,wpf_space_down));CHECK(retained_page_is(0));
    CHECK(key_down(VK_SPACE,wpf_space_down));CHECK(retained_page_is(1));
    CHECK(key_up(VK_SPACE,wpf_space_up));retained_tone_candidates->Release();
    CHECK(press(VK_ESCAPE));CHECK(context_is(expected));
    CHECK(press('A')&&press(VK_SPACE));
    expected+=L"\u5c0d";CHECK(context_is(expected));
    CHECK(begin_zhuyin()&&press('V')&&press('U')&&press('8')&&key_down(VK_SPACE));
    auto* candidates=find_candidate_ui(manager);CHECK(candidates!=nullptr);
    UINT count=0;CHECK(candidates->GetCount(&count)==S_OK&&count==12);
    UINT page=99;CHECK(candidate_page(candidates,&page)&&page==0);
    std::vector<UINT> starts;CHECK(candidate_pages(candidates,&starts)&&starts.size()==2&&starts[0]==0&&starts[1]==10);
    for(UINT i=0;i<count;++i)CHECK(candidate_annotation(candidate_string(candidates,i)));
    const auto first_candidate=candidate_text_only(candidate_string(candidates,0));
    CHECK(!first_candidate.empty());
    BOOL ctrl_eaten=TRUE,alt_eaten=TRUE;
    CHECK(service->test_key_down_for_state(context,VK_SPACE,static_cast<LPARAM>(0x40000000),false,true,false,&ctrl_eaten)==S_OK&&!ctrl_eaten);
    CHECK(service->test_key_down_for_state(context,VK_SPACE,static_cast<LPARAM>(0x40000000),false,false,true,&alt_eaten)==S_OK&&!alt_eaten);
    ctrl_eaten=TRUE;alt_eaten=TRUE;
    CHECK(service->key_down_for_state(context,VK_SPACE,false,true,false,&ctrl_eaten,false,static_cast<LPARAM>(0x40000000))==S_OK&&!ctrl_eaten);
    CHECK(service->key_down_for_state(context,VK_SPACE,false,false,true,&alt_eaten,false,static_cast<LPARAM>(0x40000000))==S_OK&&!alt_eaten);
    CHECK(key_down(VK_SPACE,static_cast<LPARAM>(0x40000000)));
    CHECK(candidate_page(candidates,&page)&&page==0);
    key_up(VK_SPACE);
    CHECK(press(VK_SPACE));CHECK(candidate_page(candidates,&page)&&page==1);
    candidates->Release();
    CHECK(press(VK_ESCAPE));CHECK(context_is(expected));
    CHECK(service->menu_command(boshiamy::MenuCommand::Fullwidth)==S_OK);
    CHECK(begin_zhuyin()&&press('V')&&press('U')&&press('8')&&press(VK_SPACE));
    CHECK(shifted_letter('A',false,true));expected+=L"\uff21";CHECK(context_is(expected));
    CHECK(service->menu_command(boshiamy::MenuCommand::Halfwidth)==S_OK);
    CHECK(begin_zhuyin()&&press('V')&&press('U')&&press('8')&&press(VK_SPACE));
    candidates=find_candidate_ui(manager);CHECK(candidates!=nullptr);
    const auto second_candidate=candidate_text_only(candidate_string(candidates,1));
    CHECK(!second_candidate.empty());
    CHECK(press(VK_DOWN));UINT selection=99;CHECK(candidates->GetSelection(&selection)==S_OK&&selection==1);
    CHECK(press(VK_RETURN));candidates->Release();
    expected+=second_candidate;CHECK(context_is(expected));
    CHECK(press('A')&&press(VK_SPACE));
    expected+=L"\u5c0d";CHECK(context_is(expected));
    CHECK(begin_zhuyin()&&press('V')&&press('U')&&press('8')&&press(VK_SPACE));
    candidates=find_candidate_ui(manager);CHECK(candidates!=nullptr);
    const auto zero_candidate=candidate_text_only(candidate_string(candidates,0));CHECK(!zero_candidate.empty());
    CHECK(press('0'));candidates->Release();
    expected+=zero_candidate;CHECK(context_is(expected));
    CHECK(press('O')&&press('E')&&press('K')&&press(VK_SPACE));
    expected+=L"\u5638";CHECK(context_is(expected));
    CHECK(doctor_queue.set_enabled(false));
    UnmapViewOfFile(doctor_view);CloseHandle(doctor_mapping);
    CHECK(service->Deactivate()==S_OK);service->Release();
    CHECK(document->Pop(TF_POPF_ALL)==S_OK);context->Release();document->Release();
    CHECK(manager->Deactivate()==S_OK);manager->Release();CoUninitialize();
    CHECK(DllCanUnloadNow()==S_FALSE);
    pending_scope_session->Release();
    pending_scope_pin.reset();
    CHECK(DllCanUnloadNow()==S_OK);
    for(const auto& entry:std::filesystem::directory_iterator(legacy_directory))
        CHECK(entry.path().filename().wstring().rfind(L"tsf-diagnostics-",0)!=0);
    std::cout << "UI-less TSF fixture routed simulated Zhuyin key events, tones, annotations, candidate navigation, repeats and following input; no TIP was registered\n";
    return 0;
}
