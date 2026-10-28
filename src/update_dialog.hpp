#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_update.hpp"
#include <commctrl.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Visible update menu for the Windows launcher (no console). It shows the
// installed version, the check/download/install state, short release notes and
// four buttons: Buscar actualizaciones, Actualizar ahora, Más tarde and Omitir
// esta versión. Downloads and installation run in a worker thread, so the
// window stays responsive and can show progress.
//
// The update is installed only from the launcher, never from the game: if the
// game is open, the player closes it and opens the launcher again. The F9 menu
// inside the game belongs to a later phase. userdata/ and config/ are never
// touched by install_update(): saves and settings survive every update.
namespace kon {
struct UpdateDialogResult {
    enum class Action {Later,Skipped,Installed};
    Action action=Action::Later;
    std::string tag;
};

namespace update_dialog_detail {
constexpr int kSearchButton=1001;
constexpr int kInstallButton=1002;
constexpr int kLaterButton=1003;
constexpr int kSkipButton=1004;
constexpr UINT kSearchMessage=WM_APP+71;
constexpr UINT kInstallProgress=WM_APP+72;
constexpr UINT kInstallResult=WM_APP+73;

struct SharedState {
    std::mutex mutex;
    bool alive=true;
};

struct SearchOutcome {
    std::optional<ReleaseInfo> release;
    std::wstring error;
};

struct ProgressMessage {
    InstallPhase phase=InstallPhase::Downloading;
    std::uint64_t done=0,total=0;
};

struct InstallOutcome {
    bool ok=false;
    std::wstring error;
};

inline bool post(HWND window,const std::shared_ptr<SharedState>& shared,UINT message,WPARAM wparam,LPARAM lparam){
    std::lock_guard<std::mutex> lock(shared->mutex);
    if(!shared->alive || !window || !IsWindow(window))return false;
    return PostMessageW(window,message,wparam,lparam)!=0;
}
inline void post_search(HWND window,const std::shared_ptr<SharedState>& shared,SearchOutcome* outcome){
    if(!post(window,shared,kSearchMessage,0,reinterpret_cast<LPARAM>(outcome)))delete outcome;
}
inline void post_progress(HWND window,const std::shared_ptr<SharedState>& shared,InstallPhase phase,std::uint64_t done,std::uint64_t total){
    auto* message=new ProgressMessage{phase,done,total};
    if(!post(window,shared,kInstallProgress,0,reinterpret_cast<LPARAM>(message)))delete message;
}
inline void post_install(HWND window,const std::shared_ptr<SharedState>& shared,InstallOutcome* outcome){
    if(!post(window,shared,kInstallResult,0,reinterpret_cast<LPARAM>(outcome)))delete outcome;
}

inline void search_worker(HWND window,const std::shared_ptr<SharedState>& shared){
    auto* outcome=new SearchOutcome;
    try{
        std::wstring error;
        outcome->release=latest_release(10000,&error);
        outcome->error=std::move(error);
    }catch(const std::exception& e){outcome->error=widen(e.what());}
    catch(...){outcome->error=L"No se pudo comprobar la actualización.";}
    post_search(window,shared,outcome);
}
inline void install_worker(std::filesystem::path root,ReleaseInfo info,HWND window,const std::shared_ptr<SharedState>& shared){
    auto* outcome=new InstallOutcome;
    try{
        install_update(root,info,[&](InstallPhase phase,std::uint64_t done,std::uint64_t total){
            post_progress(window,shared,phase,done,total);
        });
        outcome->ok=true;
    }catch(const std::exception& e){outcome->error=widen(e.what());}
    catch(...){outcome->error=L"Error desconocido al instalar la actualización.";}
    post_install(window,shared,outcome);
}
inline std::wstring windows_lines(const std::wstring& text){
    std::wstring out;out.reserve(text.size()+8);
    for(std::size_t i=0;i<text.size();++i){
        if(text[i]=='\n' && (i==0 || text[i-1]!='\r'))out+='\r';
        out+=text[i];
    }
    return out;
}
inline std::wstring shorten(std::wstring text,std::size_t max_chars,std::size_t max_lines){
    std::wstring out;std::size_t lines=1;
    for(const auto c:text){
        if(c=='\r')continue;
        if(out.size()>=max_chars || (c=='\n' && lines>=max_lines)){out+=L"\n…";break;}
        if(c=='\n')++lines;
        out+=c;
    }
    return out;
}
inline std::wstring notes_text(const ReleaseInfo& release){
    std::wstring text=L"Novedades de "+widen(release.tag)+L":\n\n";
    if(release.notes.empty())text+=L"(Esta versión no incluye notas.)";
    else text+=shorten(widen(release.notes),900,10);
    return text;
}
}  // namespace update_dialog_detail

class UpdateDialog {
public:
    UpdateDialog(std::filesystem::path root,std::optional<ReleaseInfo> initial,bool auto_install)
        : root_(std::move(root)),initial_(std::move(initial)),auto_install_(auto_install),
          shared_(std::make_shared<update_dialog_detail::SharedState>()){
        if(HDC dc=GetDC(nullptr)){dpi_=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);}
        if(dpi_<96)dpi_=96;
    }
    ~UpdateDialog(){
        if(search_thread_.joinable())search_thread_.detach();  // only posts to SharedState after this
        if(install_thread_.joinable())install_thread_.join();
        if(font_)DeleteObject(font_);
        if(bold_)DeleteObject(bold_);
        if(small_)DeleteObject(small_);
        if(white_)DeleteObject(white_);
    }
    UpdateDialog(const UpdateDialog&)=delete;
    UpdateDialog& operator=(const UpdateDialog&)=delete;

    std::optional<UpdateDialogResult> run();

private:
    int scale(int value)const{return MulDiv(value,dpi_,96);}
    HFONT make_font(int points,int weight)const{
        return CreateFontW(-MulDiv(points,dpi_,72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    }
    bool create_controls();
    void layout_controls();
    void set_status(const std::wstring& text);
    void set_notes(const std::wstring& text);
    void set_marquee(bool on);
    void set_progress(std::uint64_t done,std::uint64_t total);
    void update_controls();
    void start_search();
    void start_install();
    void apply_release(const ReleaseInfo& release);
    void maybe_auto_install();
    void on_search_result(update_dialog_detail::SearchOutcome& outcome);
    void on_progress(const update_dialog_detail::ProgressMessage& progress);
    void on_install_result(update_dialog_detail::InstallOutcome& outcome);
    void close_later();
    void drain_messages();

    static LRESULT CALLBACK window_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam);

    std::filesystem::path root_;
    std::optional<ReleaseInfo> initial_;
    std::optional<ReleaseInfo> release_;
    bool auto_install_=false,auto_install_done_=false;
    bool update_available_=false,busy_=false,installing_=false,finished_=false;
    std::optional<UpdateDialogResult> result_;
    int dpi_=96;
    HWND hwnd_=nullptr,version_=nullptr,status_=nullptr,notes_=nullptr,footer_=nullptr,footnote_=nullptr,progress_=nullptr;
    HWND search_=nullptr,install_=nullptr,later_=nullptr,skip_=nullptr;
    HFONT font_=nullptr,bold_=nullptr,small_=nullptr;
    HBRUSH white_=nullptr;
    std::shared_ptr<update_dialog_detail::SharedState> shared_;
    std::thread search_thread_,install_thread_;
    std::uint64_t last_download_chunk_=~std::uint64_t(0);
};

inline LRESULT CALLBACK UpdateDialog::window_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
    auto* self=reinterpret_cast<UpdateDialog*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    switch(message){
        case WM_NCCREATE:{
            auto* create=reinterpret_cast<CREATESTRUCTW*>(lparam);
            self=static_cast<UpdateDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
            if(self)self->hwnd_=hwnd;
            return TRUE;
        }
        case WM_CREATE:
            if(!self || !self->create_controls())return -1;
            return 0;
        case WM_COMMAND:
            if(!self)break;
            switch(LOWORD(wparam)){
                case IDCANCEL:self->close_later();return 0;
                case update_dialog_detail::kSearchButton:self->start_search();return 0;
                case update_dialog_detail::kInstallButton:self->start_install();return 0;
                case update_dialog_detail::kLaterButton:self->close_later();return 0;
                case update_dialog_detail::kSkipButton:
                    if(self->update_available_ && !self->busy_ && self->release_){
                        self->result_=UpdateDialogResult{UpdateDialogResult::Action::Skipped,self->release_->tag};
                        DestroyWindow(hwnd);
                    }
                    return 0;
            }
            break;
        case WM_CLOSE:
            if(self)self->close_later();
            return 0;
        case WM_KEYDOWN:
            if(self && wparam==VK_ESCAPE){self->close_later();return 0;}
            break;
        case WM_TIMER:
            if(self && wparam==1){KillTimer(hwnd,1);DestroyWindow(hwnd);return 0;}
            break;
        case update_dialog_detail::kSearchMessage:
            if(self){
                std::unique_ptr<update_dialog_detail::SearchOutcome> outcome(
                    reinterpret_cast<update_dialog_detail::SearchOutcome*>(lparam));
                self->on_search_result(*outcome);
            }
            return 0;
        case update_dialog_detail::kInstallProgress:
            if(self){
                std::unique_ptr<update_dialog_detail::ProgressMessage> progress(
                    reinterpret_cast<update_dialog_detail::ProgressMessage*>(lparam));
                self->on_progress(*progress);
            }
            return 0;
        case update_dialog_detail::kInstallResult:
            if(self){
                std::unique_ptr<update_dialog_detail::InstallOutcome> outcome(
                    reinterpret_cast<update_dialog_detail::InstallOutcome*>(lparam));
                self->on_install_result(*outcome);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            if(self){
                auto* hdc=reinterpret_cast<HDC>(wparam);
                SetBkMode(hdc,TRANSPARENT);
                if(reinterpret_cast<HWND>(lparam)==self->footnote_)SetTextColor(hdc,GetSysColor(COLOR_GRAYTEXT));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        case WM_CTLCOLOREDIT:
            if(self){
                auto* hdc=reinterpret_cast<HDC>(wparam);
                SetBkColor(hdc,RGB(255,255,255));SetTextColor(hdc,GetSysColor(COLOR_WINDOWTEXT));
                return reinterpret_cast<LRESULT>(self->white_?self->white_:GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        case WM_DESTROY:
            if(self){
                {
                    std::lock_guard<std::mutex> lock(self->shared_->mutex);
                    self->shared_->alive=false;
                }
                self->drain_messages();
                self->finished_=true;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

inline bool UpdateDialog::create_controls(){
    font_=make_font(14,FW_NORMAL);bold_=make_font(16,FW_BOLD);small_=make_font(12,FW_NORMAL);
    white_=CreateSolidBrush(RGB(255,255,255));
    const auto instance=GetModuleHandleW(nullptr);
    version_=CreateWindowExW(0,L"STATIC",L"Versión instalada: " KON_VERSION_WIDE,
        WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,hwnd_,nullptr,instance,nullptr);
    status_=CreateWindowExW(0,L"STATIC",L"Preparando…",
        WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,hwnd_,nullptr,instance,nullptr);
    notes_=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Tus partidas y ajustes se conservan.",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        0,0,0,0,hwnd_,nullptr,instance,nullptr);
    footer_=CreateWindowExW(0,L"STATIC",L"Tus partidas y ajustes se conservan.",
        WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,hwnd_,nullptr,instance,nullptr);
    footnote_=CreateWindowExW(0,L"STATIC",
        L"La actualización se instala desde el lanzador, no desde el juego. Si el juego está abierto, ciérralo antes de actualizar.",
        WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,hwnd_,nullptr,instance,nullptr);
    progress_=CreateWindowExW(0,PROGRESS_CLASSW,nullptr,WS_CHILD|PBS_SMOOTH,0,0,0,0,hwnd_,nullptr,instance,nullptr);
    search_=CreateWindowExW(0,L"BUTTON",L"Buscar actualizaciones",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
        0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(update_dialog_detail::kSearchButton)),instance,nullptr);
    install_=CreateWindowExW(0,L"BUTTON",L"Actualizar ahora",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
        0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(update_dialog_detail::kInstallButton)),instance,nullptr);
    later_=CreateWindowExW(0,L"BUTTON",L"Más tarde",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
        0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(update_dialog_detail::kLaterButton)),instance,nullptr);
    skip_=CreateWindowExW(0,L"BUTTON",L"Omitir esta versión",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
        0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(update_dialog_detail::kSkipButton)),instance,nullptr);
    if(!version_||!status_||!notes_||!footer_||!footnote_||!progress_||!search_||!install_||!later_||!skip_)return false;
    SendMessageW(version_,WM_SETFONT,reinterpret_cast<WPARAM>(bold_),TRUE);
    SendMessageW(status_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    SendMessageW(notes_,WM_SETFONT,reinterpret_cast<WPARAM>(small_),TRUE);
    SendMessageW(footer_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    SendMessageW(footnote_,WM_SETFONT,reinterpret_cast<WPARAM>(small_),TRUE);
    for(HWND button:{search_,install_,later_,skip_})SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(small_),TRUE);
    SendMessageW(notes_,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(scale(6),scale(6)));
    SendMessageW(progress_,PBM_SETRANGE32,0,1000);
    layout_controls();
    SetWindowTextW(version_,L"Versión instalada: " KON_VERSION_WIDE);
    return true;
}

inline void UpdateDialog::layout_controls(){
    const int margin=scale(18);int y=scale(14);
    MoveWindow(version_,margin,y,scale(624),scale(24),TRUE);y+=scale(30);
    MoveWindow(status_,margin,y,scale(624),scale(60),TRUE);y+=scale(66);
    MoveWindow(notes_,margin,y,scale(624),scale(160),TRUE);y+=scale(168);
    MoveWindow(footer_,margin,y,scale(624),scale(22),TRUE);y+=scale(26);
    MoveWindow(footnote_,margin,y,scale(624),scale(44),TRUE);y+=scale(50);
    MoveWindow(progress_,margin,y,scale(624),scale(8),TRUE);y+=scale(20);
    MoveWindow(search_,margin,y,scale(170),scale(34),TRUE);
    MoveWindow(install_,margin+scale(178),y,scale(140),scale(34),TRUE);
    MoveWindow(skip_,margin+scale(326),y,scale(160),scale(34),TRUE);
    MoveWindow(later_,margin+scale(494),y,scale(90),scale(34),TRUE);
    ShowWindow(progress_,SW_HIDE);
}

inline void UpdateDialog::set_status(const std::wstring& text){
    if(status_)SetWindowTextW(status_,update_dialog_detail::windows_lines(text).c_str());
}
inline void UpdateDialog::set_notes(const std::wstring& text){
    if(notes_)SetWindowTextW(notes_,update_dialog_detail::windows_lines(text).c_str());
}
inline void UpdateDialog::set_marquee(bool on){
    if(!progress_)return;
    ShowWindow(progress_,SW_SHOW);
    SendMessageW(progress_,PBM_SETMARQUEE,on?TRUE:FALSE,on?24:0);
}
inline void UpdateDialog::set_progress(std::uint64_t done,std::uint64_t total){
    if(!progress_)return;
    ShowWindow(progress_,SW_SHOW);
    SendMessageW(progress_,PBM_SETMARQUEE,FALSE,0);
    const int position=total?int((done*1000)/total):0;
    SendMessageW(progress_,PBM_SETPOS,position,0);
}
inline void UpdateDialog::update_controls(){
    const bool can_act=!busy_&&!finished_;
    EnableWindow(search_,can_act);
    EnableWindow(install_,can_act&&update_available_&&release_.has_value());
    EnableWindow(skip_,can_act&&update_available_&&release_.has_value());
    EnableWindow(later_,!installing_&&!finished_);
}

inline void UpdateDialog::start_search(){
    if(busy_||installing_)return;
    if(search_thread_.joinable())search_thread_.join();
    if(install_thread_.joinable())install_thread_.join();  // a previous failed install has already posted its result
    busy_=true;update_available_=false;release_.reset();last_download_chunk_=~std::uint64_t(0);
    set_status(L"Buscando actualizaciones…");
    set_notes(L"Comprobando si hay una versión nueva en GitHub. Si no hay conexión, el juego se abre igualmente.");
    set_marquee(true);
    update_controls();
    search_thread_=std::thread([window=hwnd_,shared=shared_]{update_dialog_detail::search_worker(window,shared);});
}

inline void UpdateDialog::start_install(){
    if(busy_||!release_||!update_available_||finished_)return;
    if(install_thread_.joinable())install_thread_.join();
    auto info=*release_;
    busy_=true;installing_=true;auto_install_done_=true;last_download_chunk_=~std::uint64_t(0);
    set_status(L"Descargando "+widen(info.tag)+L"…");
    set_marquee(true);
    update_controls();
    install_thread_=std::thread([root=root_,info=std::move(info),window=hwnd_,shared=shared_]{
        update_dialog_detail::install_worker(root,std::move(info),window,shared);
    });
}

inline void UpdateDialog::maybe_auto_install(){
    if(auto_install_ && !auto_install_done_ && update_available_ && release_ && !busy_){
        auto_install_done_=true;
        start_install();
    }
}

inline void UpdateDialog::apply_release(const ReleaseInfo& release){
    release_=release;
    const auto latest=Version::parse(release.tag),current=Version::parse(KON_VERSION_STRING);
    if(latest && current && *current<*latest){
        update_available_=true;
        set_status(L"Hay una versión nueva: "+widen(release.tag));
        set_notes(update_dialog_detail::notes_text(release));
        if(progress_)ShowWindow(progress_,SW_HIDE);
    }else{
        update_available_=false;
        if(!latest)set_status(L"No se pudo leer la versión publicada ("+widen(release.tag)+L").");
        else if(current && *latest<*current)set_status(L"Tu versión (" KON_VERSION_WIDE L") es más reciente que la publicada ("+widen(release.tag)+L").");
        else set_status(L"Tienes la última versión ("+widen(release.tag)+L").");
        set_notes(L"No hay novedades que instalar.\n\nTus partidas y ajustes se conservan.");
        if(progress_)ShowWindow(progress_,SW_HIDE);
    }
    update_controls();
    maybe_auto_install();
}

inline void UpdateDialog::on_search_result(update_dialog_detail::SearchOutcome& outcome){
    busy_=false;
    set_marquee(false);
    if(outcome.release){
        apply_release(*outcome.release);
        return;
    }
    update_available_=false;
    if(!outcome.error.empty())set_status(L"Error al buscar actualizaciones:\n"+outcome.error);
    else set_status(L"No se pudo comprobar si hay actualizaciones.");
    set_notes(L"Puedes seguir jugando con la versión instalada. Vuelve a intentarlo cuando tengas conexión.\n\nTus partidas y ajustes se conservan.");
    if(progress_)ShowWindow(progress_,SW_HIDE);
    update_controls();
}

inline void UpdateDialog::on_progress(const update_dialog_detail::ProgressMessage& progress){
    if(!installing_)return;
    switch(progress.phase){
        case InstallPhase::Downloading:{
            const auto chunk=progress.done>>10;  // update the text every KB, not every packet
            if(chunk!=last_download_chunk_){
                last_download_chunk_=chunk;
                const auto size_text=[](std::uint64_t bytes){
                    if(bytes>=(1u<<20))return std::to_wstring(bytes>>20)+L" MB";
                    return std::to_wstring((bytes+1023)>>10)+L" KB";
                };
                std::wstring text=L"Descargando "+widen(release_?release_->tag:std::string())+L"…";
                if(progress.total)text+=L"  "+size_text(progress.done)+L" de "+size_text(progress.total);
                else if(progress.done)text+=L"  "+size_text(progress.done);
                set_status(text);
            }
            set_progress(progress.done,progress.total);
            break;
        }
        case InstallPhase::Extracting:
            set_status(L"Descomprimiendo la actualización…");
            set_marquee(true);
            break;
        case InstallPhase::Installing:
            set_status(L"Instalando "+widen(release_?release_->tag:std::string())+L"…");
            if(progress.total)set_progress(progress.done,progress.total);
            else set_marquee(true);
            break;
        case InstallPhase::Done:
            set_progress(1,1);
            break;
    }
}

inline void UpdateDialog::on_install_result(update_dialog_detail::InstallOutcome& outcome){
    busy_=false;installing_=false;
    set_marquee(false);
    if(outcome.ok){
        set_status(L"Actualización instalada. Reiniciando el lanzador…");
        set_notes(L"La nueva versión se abrirá ahora.\n\nTus partidas y ajustes se conservan.");
        result_=UpdateDialogResult{UpdateDialogResult::Action::Installed,release_?release_->tag:std::string()};
        finished_=true;
        set_progress(1,1);
        update_controls();
        SetTimer(hwnd_,1,1200,nullptr);
    }else{
        set_status(L"Error al instalar la actualización:\n"+(outcome.error.empty()?L"No se pudo completar.":outcome.error));
        set_notes(L"No se ha cambiado nada. Comprueba la conexión y el espacio libre en el disco, y vuelve a intentarlo.\n\nTus partidas y ajustes se conservan.");
        if(progress_)ShowWindow(progress_,SW_HIDE);
        update_controls();
    }
}

inline void UpdateDialog::close_later(){
    if(installing_){
        set_status(L"Instalando la actualización. Espera a que termine…");
        return;
    }
    if(!result_)result_=UpdateDialogResult{UpdateDialogResult::Action::Later,release_?release_->tag:std::string()};
    DestroyWindow(hwnd_);
}

inline void UpdateDialog::drain_messages(){
    MSG message{};
    while(PeekMessageW(&message,hwnd_,update_dialog_detail::kSearchMessage,update_dialog_detail::kInstallResult,PM_REMOVE)){
        if(message.message==update_dialog_detail::kInstallProgress)delete reinterpret_cast<update_dialog_detail::ProgressMessage*>(message.lParam);
        else if(message.message==update_dialog_detail::kSearchMessage)delete reinterpret_cast<update_dialog_detail::SearchOutcome*>(message.lParam);
        else if(message.message==update_dialog_detail::kInstallResult)delete reinterpret_cast<update_dialog_detail::InstallOutcome*>(message.lParam);
    }
}

inline std::optional<UpdateDialogResult> UpdateDialog::run(){
    INITCOMMONCONTROLSEX common{sizeof common,ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&common);
    WNDCLASSEXW window_class{sizeof window_class};
    window_class.style=CS_HREDRAW|CS_VREDRAW;
    window_class.lpfnWndProc=&UpdateDialog::window_proc;
    window_class.hInstance=GetModuleHandleW(nullptr);
    window_class.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));  // IDC_ARROW
    window_class.hIcon=LoadIconW(window_class.hInstance,MAKEINTRESOURCEW(1));
    if(!window_class.hIcon)window_class.hIcon=LoadIconW(nullptr,MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
    window_class.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName=L"KOnUpdateDialog";
    RegisterClassExW(&window_class);  // a second call in the same process fails harmlessly
    RECT rect{0,0,scale(660),scale(430)};
    AdjustWindowRectEx(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,FALSE,WS_EX_CONTROLPARENT|WS_EX_APPWINDOW);
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    const int x=(GetSystemMetrics(SM_CXSCREEN)-width)/2,y=(GetSystemMetrics(SM_CYSCREEN)-height)/2;
    hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_APPWINDOW,L"KOnUpdateDialog",
        L"Actualizaciones - K-On! Houkago Live!! Recompiled",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,x,y,width,height,
        nullptr,nullptr,window_class.hInstance,this);
    if(!hwnd_)return std::nullopt;
    SetWindowTextW(hwnd_,L"Actualizaciones - K-On! Houkago Live!! Recompiled");
    ShowWindow(hwnd_,SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);  // visible even when the launcher was started from a console
    UpdateWindow(hwnd_);
    if(initial_)apply_release(*initial_);
    else start_search();
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){
        if(!IsDialogMessageW(hwnd_,&message)){
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result_;
}

inline std::optional<UpdateDialogResult> run_update_dialog(const std::filesystem::path& root,
        std::optional<ReleaseInfo> initial,bool auto_install){
    UpdateDialog dialog(root,std::move(initial),auto_install);
    return dialog.run();
}
}  // namespace kon
