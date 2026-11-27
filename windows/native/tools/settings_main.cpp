#include "settings_window.hpp"
#include "../doctor_poc.hpp"
#include "../doctor_host.hpp"
#include "../build_info.hpp"
#include "../doctor_paths.hpp"
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
 int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 if(!argv){MessageBoxW(nullptr,L"無法解析命令列參數。",L"嘸蝦米輸入法 設定",MB_ICONERROR);return 1;}
 struct Arguments {LPWSTR* value;~Arguments(){LocalFree(value);}} arguments{argv};
 if(argc>1&&std::wstring(argv[1])==L"--doctor-collector")
  return argc==2?boshiamy::doctor::doctor_host::run_collector():1;
 if(argc>1&&std::wstring(argv[1])==L"--doctor-once")
  return argc==2?boshiamy::doctor::doctor_host::run_once():1;
 if(argc>1&&std::wstring(argv[1])==boshiamy::doctor::kDoctorMaintenanceStopUpgradeArgument)
  return argc==2?boshiamy::doctor::doctor_host::run_maintenance(boshiamy::doctor::DoctorHostMaintenanceAction::StopForUpgrade):1;
 if(argc>1&&std::wstring(argv[1])==boshiamy::doctor::kDoctorMaintenanceStopRemovalArgument)
  return argc==2?boshiamy::doctor::doctor_host::run_maintenance(boshiamy::doctor::DoctorHostMaintenanceAction::StopForRemoval):1;
 if(argc>1&&std::wstring(argv[1])==boshiamy::doctor::kDoctorMaintenanceResumeArgument)
  return argc==2?boshiamy::doctor::doctor_host::run_maintenance(boshiamy::doctor::DoctorHostMaintenanceAction::ResumeAfterUpgrade):1;
 if(argc>1&&boshiamy::doctor::poc::is_doctor_argument(argv[1]))
  return boshiamy::doctor::poc::run(argc,argv);
 const bool doctor_settings=argc==2&&std::wstring(argv[1])==L"--doctor-settings";
 if(argc==2&&std::wstring(argv[1])==L"--doctor-folder"){
  try{
   const auto folder=boshiamy::doctor::doctor_directory();
   std::error_code error;
   if(!std::filesystem::is_directory(folder,error)||error)
    throw std::filesystem::filesystem_error("Doctor folder is unavailable",folder,error);
   if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32)return 0;
  }catch(...){}
  MessageBoxW(nullptr,L"無法開啟 Doctor 資料夾。",L"嘸．改 Doctor",MB_ICONERROR);return 1;
 }
 std::filesystem::path path;
 try{path=argc>1&&!doctor_settings?std::filesystem::path(argv[1]):boshiamy::settings_file_path();}
 catch(...){MessageBoxW(nullptr,L"找不到本機設定資料夾。",L"嘸蝦米輸入法 設定",MB_ICONERROR);return 1;}
 boshiamy::SettingsWindowOptions options;
 if(doctor_settings)options.initial_page=boshiamy::SettingsWindowPage::Doctor;
 boshiamy::SettingsWindow window(instance,path,[]{PostQuitMessage(0);},std::move(options));
 if(!window.create()){MessageBoxW(nullptr,L"無法開啟設定視窗。",L"嘸蝦米輸入法 設定",MB_ICONERROR);return 1;}
 window.show(show);MSG message{};int result;
 while((result=GetMessageW(&message,nullptr,0,0))>0){
  if(!window.hwnd()||!IsDialogMessageW(window.hwnd(),&message)){TranslateMessage(&message);DispatchMessageW(&message);}
 }
 return result<0?1:static_cast<int>(message.wParam);
}
