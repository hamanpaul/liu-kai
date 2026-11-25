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
    auto* service=new Service(launch_tool_process,{});
    CHECK(service->ActivateEx(manager,client,TF_TMAE_UIELEMENTENABLEDONLY)==S_OK);
    CHECK(service->menu_command(boshiamy::MenuCommand::Chinese)==S_OK);
    CHECK(service->menu_command(boshiamy::MenuCommand::Traditional)==S_OK);
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
    CHECK(press('O') && press('E') && press('K') && press(VK_SPACE));
    CHECK(read_context(context,client,&text)==S_OK && text==L"\u5c0d\u5638");
    if(argc==3) {
        CHECK(service->Deactivate()==S_OK);service->Release();
        CHECK(document->Pop(TF_POPF_ALL)==S_OK);context->Release();document->Release();
        CHECK(manager->Deactivate()==S_OK);manager->Release();CoUninitialize();
        CHECK(DllCanUnloadNow()==S_OK);
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
    CHECK(service->Deactivate()==S_OK);service->Release();
    CHECK(document->Pop(TF_POPF_ALL)==S_OK);context->Release();document->Release();
    CHECK(manager->Deactivate()==S_OK);manager->Release();CoUninitialize();
    CHECK(DllCanUnloadNow()==S_OK);
    std::cout << "UI-less TSF fixture routed simulated Zhuyin key events, tones, annotations, candidate navigation, repeats and following input; no TIP was registered\n";
    return 0;
}
