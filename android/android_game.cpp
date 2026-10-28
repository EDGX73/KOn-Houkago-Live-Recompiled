// Android entry point and host glue for the recompiled game.
//
// The game runtime (src/boot_probe.cpp, compiled as kon_main) renders into its
// own CPU framebuffer. GameActivity owns a SurfaceView; the JNI surface
// callbacks below hand its ANativeWindow to NativeWindow, which copies each
// finished frame. Input arrives from Java as a PSP pad bitmask (touch overlay
// or a controller); text input is delegated back to an EditText dialog.
#include "native_display_android.hpp"
#include "game_module_abi.hpp"
#include "native_gamepads.hpp"
#include "kon_version.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <jni.h>
#include <unistd.h>
#include <dlfcn.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int kon_main(int argc,char** argv);

extern "C" void (*kon_android_game_register)(psprecomp::Runtime&) = nullptr;
#define KON_LOG(...) __android_log_print(ANDROID_LOG_INFO,"kon",__VA_ARGS__)

namespace kon::android_window {
namespace {
std::mutex surface_mutex;
ANativeWindow* window=nullptr;
std::atomic<bool> focused_{true},closed_{false};
std::atomic<std::uint32_t> keys_{0};
NativeWindow* active=nullptr;
JavaVM* vm=nullptr;
jobject activity=nullptr;
std::thread game_thread;
std::mutex game_mutex;

std::string utf8(JNIEnv* env,jstring value){
    if(!value)return {};
    const char* text=env->GetStringUTFChars(value,nullptr);
    if(!text)return {};
    std::string out(text);
    env->ReleaseStringUTFChars(value,text);
    return out;
}

std::wstring widen(const std::string& text){
    std::wstring out;out.reserve(text.size());
    for(std::size_t i=0;i<text.size();){
        const auto c=static_cast<unsigned char>(text[i++]);std::uint32_t v=c;
        if(c>=0x80){
            unsigned extra=c<0xE0?1:c<0xF0?2:3;v=c&(0x3F>>extra);
            if(i+extra>text.size()){out+=L'?';break;}
            for(unsigned n=0;n<extra;++n)v=(v<<6)|(static_cast<unsigned char>(text[i++])&0x3F);
        }
        out+=static_cast<wchar_t>(v);
    }
    return out;
}

std::string narrow(const std::wstring& text){
    std::string out;out.reserve(text.size());
    for(const wchar_t c:text){
        auto v=static_cast<std::uint32_t>(c);
        if(v<0x80)out+=char(v);
        else if(v<0x800){out+=char(0xC0|(v>>6));out+=char(0x80|(v&0x3F));}
        else if(v<0x10000){out+=char(0xE0|(v>>12));out+=char(0x80|((v>>6)&0x3F));out+=char(0x80|(v&0x3F));}
        else{out+=char(0xF0|(v>>18));out+=char(0x80|((v>>12)&0x3F));out+=char(0x80|((v>>6)&0x3F));out+=char(0x80|(v&0x3F));}
    }
    return out;
}
} // namespace

ANativeWindow* surface(){
    std::lock_guard lock(surface_mutex);
    return window;
}

void set_surface(ANativeWindow* value){
    if(value)ANativeWindow_acquire(value);
    std::lock_guard lock(surface_mutex);
    if(window)ANativeWindow_release(window);
    window=value;
    if(window)ANativeWindow_setBuffersGeometry(window,480,272,WINDOW_FORMAT_RGBA_8888);
}

void clear_surface(){
    std::lock_guard lock(surface_mutex);
    if(window){ANativeWindow_release(window);window=nullptr;}
}

void present(const std::uint32_t* pixels,unsigned width,unsigned height){
    if(!pixels || !width || !height)return;
    std::lock_guard lock(surface_mutex);
    if(!window)return;
    ANativeWindow_Buffer buffer{};
    if(ANativeWindow_lock(window,&buffer,nullptr)!=0)return;
    const unsigned rows=std::min(height,unsigned(buffer.height));
    const unsigned columns=std::min(width,unsigned(buffer.width));
    auto* destination=static_cast<std::uint32_t*>(buffer.bits);
    for(unsigned y=0;y<rows;++y){
        const auto* in=pixels+static_cast<std::size_t>(y)*width;
        auto* out=destination+static_cast<std::size_t>(y)*buffer.stride;
        for(unsigned x=0;x<columns;++x){
            const auto p=in[x];
            out[x]=(p&0xFF00FF00u)|((p&0xFFu)<<16u)|((p>>16u)&0xFFu);
        }
    }
    ANativeWindow_unlockAndPost(window);
}

bool focused(){return focused_.load(std::memory_order_relaxed);}
void set_focused(bool value){focused_.store(value,std::memory_order_relaxed);}
bool closed(){return closed_.load(std::memory_order_relaxed);}
void set_closed(bool value){closed_.store(value,std::memory_order_relaxed);}
void key_press(std::uint32_t bit){keys_.fetch_or(bit);}

void set_active_window(NativeWindow* value){
    std::lock_guard lock(game_mutex);
    active=value;
}

void clear_active_window(NativeWindow* value){
    std::lock_guard lock(game_mutex);
    if(active==value)active=nullptr;
}

void request_text(std::wstring description,std::wstring initial,unsigned limit){
    JNIEnv* env=nullptr;bool attached=false;
    if(!vm)return;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK){
        if(vm->AttachCurrentThread(&env,nullptr)!=JNI_OK)return;
        attached=true;
    }
    if(activity){
        const auto description_text=narrow(description),initial_text=narrow(initial);
        jstring d=env->NewStringUTF(description_text.c_str());
        jstring i=env->NewStringUTF(initial_text.c_str());
        jclass cls=env->GetObjectClass(activity);
        jmethodID method=env->GetMethodID(cls,"requestTextInput","(Ljava/lang/String;Ljava/lang/String;I)V");
        if(method)env->CallVoidMethod(activity,method,d,i,jint(limit));
        if(env->ExceptionCheck())env->ExceptionClear();
        if(d)env->DeleteLocalRef(d);
        if(i)env->DeleteLocalRef(i);
        env->DeleteLocalRef(cls);
    }
    if(attached)vm->DetachCurrentThread();
}

void deliver_text_result(std::wstring text,bool accepted){
    std::lock_guard lock(game_mutex);
    if(active)active->deliver_text(std::move(text),accepted);
}

void online_result(std::wstring){}
std::wstring online_action(int,const std::wstring&){return {};}
} // namespace kon::android_window

using namespace kon::android_window;

namespace {
std::string g_root;
std::string g_language;
std::string g_module_error;

void stop_game_thread(){
    set_closed(true);
    std::thread thread;
    {
        std::lock_guard lock(game_mutex);
        if(game_thread.joinable())thread=std::move(game_thread);
    }
    if(thread.joinable())thread.join();
}
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* java,void* reserved){
    (void)reserved;
    vm=java;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeStart(JNIEnv* env,jobject thiz,jstring root,jstring language,jstring module){
    const std::string module_path=utf8(env,module);
    g_module_error.clear();
    if(module_path.empty()){
        g_module_error="ruta del módulo vacía";
        KON_LOG("game module path is empty");
        return JNI_FALSE;
    }
    void* module_handle=dlopen(module_path.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!module_handle){
        const char* reason=dlerror();
        g_module_error=std::string("no se pudo cargar el módulo: ")+(reason?reason:"error desconocido");
        KON_LOG("cannot load game module %s: %s",module_path.c_str(),reason?reason:"unknown");
        return JNI_FALSE;
    }
    auto register_game=reinterpret_cast<void(*)(psprecomp::Runtime&)>(dlsym(module_handle,"kon_game_register"));
    if(!register_game){
        g_module_error="el módulo no exporta kon_game_register";
        KON_LOG("game module %s does not export kon_game_register",module_path.c_str());
        dlclose(module_handle);
        return JNI_FALSE;
    }
    auto game_abi=reinterpret_cast<const char*(*)()>(dlsym(module_handle,"kon_game_abi"));
    if(!game_abi || !game_abi() || std::strcmp(game_abi(),kGameModuleAbi)!=0){
        g_module_error=std::string("el módulo no declara la ABI ")+kGameModuleAbi+
            " (¿preparado con otra versión de la app?)";
        KON_LOG("game module %s does not declare ABI %s",module_path.c_str(),kGameModuleAbi);
        dlclose(module_handle);
        return JNI_FALSE;
    }
    kon_android_game_register=register_game;
    {
        stop_game_thread();
        std::lock_guard lock(game_mutex);
        if(activity)env->DeleteGlobalRef(activity);
        activity=env->NewGlobalRef(thiz);
    }
    g_root=utf8(env,root);
    g_language=utf8(env,language);
    if(g_root.empty())return JNI_FALSE;
    set_closed(false);
    set_focused(true);
    {
        std::lock_guard lock(game_mutex);
        game_thread=std::thread([]{
            const std::string root=g_root,language=g_language;
            KON_LOG("starting game root=%s language=%s",root.c_str(),language.c_str());
            if(chdir(root.c_str())!=0)std::fprintf(stderr,"kon: cannot chdir to %s\n",root.c_str());
            std::vector<std::string> args{"kon_game","--root",root,"--language",language,"--window"};
            std::vector<char*> argv;
            for(auto& arg:args)argv.push_back(arg.data());
            try{
                kon_main(static_cast<int>(argv.size()),argv.data());
                KON_LOG("game exited");
            }catch(const std::exception& error){
                KON_LOG("game failed: %s",error.what());
                std::fprintf(stderr,"kon: game failed: %s\n",error.what());
            }catch(...){
                KON_LOG("game failed with an unknown error");
                std::fprintf(stderr,"kon: game failed with an unknown error\n");
            }
            set_closed(true);
        });
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeStop(JNIEnv*,jobject){
    stop_game_thread();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeLastError(JNIEnv* env,jobject){
    return env->NewStringUTF(g_module_error.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeSurfaceCreated(JNIEnv* env,jobject,jobject surface){
    ANativeWindow* value=ANativeWindow_fromSurface(env,surface);
    set_surface(value);
    if(value)ANativeWindow_release(value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeSurfaceChanged(JNIEnv* env,jobject,jobject surface,jint,jint){
    ANativeWindow* value=ANativeWindow_fromSurface(env,surface);
    set_surface(value);
    if(value)ANativeWindow_release(value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeSurfaceDestroyed(JNIEnv*,jobject){
    clear_surface();
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeFocus(JNIEnv*,jobject,jboolean focused){
    set_focused(focused==JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeTouch(JNIEnv*,jobject,jint buttons,jint x,jint y,jboolean active){
    kon::PadState state;
    state.buttons=std::uint32_t(buttons);
    state.x=static_cast<std::uint8_t>(std::clamp(int(x),0,255));
    state.y=static_cast<std::uint8_t>(std::clamp(int(y),0,255));
    state.valid=active==JNI_TRUE;
    kon::GamepadHub::instance().submit(1,{"touch",0,0,kon::InputStyle::PlayStation},state);
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeGamepad(JNIEnv* env,jobject,jint device,jstring name,jint buttons,jint x,jint y,jboolean connected){
    const std::string label=utf8(env,name);
    if(connected!=JNI_TRUE){
        kon::GamepadHub::instance().remove(std::uint64_t(100+device));
        return;
    }
    kon::PadState state;
    state.buttons=std::uint32_t(buttons);
    state.x=static_cast<std::uint8_t>(std::clamp(int(x),0,255));
    state.y=static_cast<std::uint8_t>(std::clamp(int(y),0,255));
    state.valid=true;
    kon::GamepadHub::instance().submit(std::uint64_t(100+device),{label,0,0,kon::InputStyle::Xbox},state);
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeKey(JNIEnv*,jobject,jint key,jboolean down){
    if(down!=JNI_TRUE)return;
    // Menu/help shortcuts; the game's pad buttons come from nativeTouch/nativeGamepad.
    if(key==82)key_press(0x1u); // KEYCODE_MENU
}

extern "C" JNIEXPORT void JNICALL
Java_com_edgx73_konrecompiled_GameActivity_nativeTextResult(JNIEnv* env,jobject,jstring text,jboolean accepted){
    deliver_text_result(widen(utf8(env,text)),accepted==JNI_TRUE);
}
