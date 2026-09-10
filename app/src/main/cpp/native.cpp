// VDPlus — Virtual Desktop 增强 (LSPosed native hook)
// 注意: mono_ldstr_checked 真实 ABI 为 (ptr,u32,ptr,...), hook 必须用 void* 透传 x0-x3, 否则截断指针会崩溃。

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <dlfcn.h>
#include <android/log.h>
#include <jni.h>

#define TAG "VDPlus"
static void vlog(int prio,const char* fmt,...){
  va_list ap; va_start(ap,fmt);
  __android_log_vprint(prio,TAG,fmt,ap);
  va_end(ap);
}
#define LOGI(...) vlog(ANDROID_LOG_INFO,__VA_ARGS__)
#define LOGE(...) vlog(ANDROID_LOG_ERROR,__VA_ARGS__)

// ---- LSPosed Native Hook API 类型 ----
typedef int (*HookFunType)(void *func, void *replace, void **backup);
typedef int (*UnhookFunType)(void *func);
typedef void (*NativeOnModuleLoaded)(const char *name, void *handle);
typedef struct { uint32_t version; HookFunType hook_func; UnhookFunType unhook_func; } NativeAPIEntries;
typedef NativeOnModuleLoaded (*NativeInit)(const NativeAPIEntries *entries);

static HookFunType g_hook = nullptr;
static int hook(void* target, void* replace, void** orig){
  return g_hook ? g_hook(target, replace, orig) : -1;
}

// ---- Mono 类型(仅作指针用途) ----
struct _MonoObject; struct _MonoImage; struct _MonoClass; struct _MonoMethod;
struct _MonoString; struct _MonoType; struct _MonoDomain;
typedef struct _MonoObject MonoObject; typedef struct _MonoImage MonoImage;
typedef struct _MonoClass MonoClass; typedef struct _MonoMethod MonoMethod;
typedef struct _MonoString MonoString; typedef struct _MonoType MonoType;
typedef struct _MonoDomain MonoDomain;
typedef struct _MonoClassField* MonoField;
typedef uint32_t MonoGCHandle;

// ---- Mono 导出符号 ----
static MonoImage*  (*pGetCorlib)(void)=nullptr;
static MonoClass*  (*pClassFromName)(MonoImage*,const char*,const char*)=nullptr;
static MonoMethod* (*pClassGetMethod)(MonoClass*,const char*,int)=nullptr;
static MonoObject* (*pRuntimeInvoke)(MonoMethod*,void*,void**,MonoObject**)=nullptr;
static MonoClass*  (*pTypeGetClass)(MonoType*)=nullptr;
static void*       (*pCompileMethod)(MonoMethod*)=nullptr;
static MonoClass*  (*pObjGetClass)(MonoObject*)=nullptr;
static void*       (*pObjUnbox)(MonoObject*)=nullptr;
static MonoClass*  (*pClassGetParent)(MonoClass*)=nullptr;
static MonoField   (*pClassGetField)(MonoClass*,const char*)=nullptr;
static void        (*pFieldGetValue)(void*,void*,void*)=nullptr;
static void        (*pFieldSetValue)(void*,MonoField,void*)=nullptr;
static MonoString* (*pLdstrChecked)(void*,void*,uint32_t,void*)=nullptr;
static MonoString* (*pStringNewUtf16)(MonoDomain*,const uint16_t*,int32_t)=nullptr;
static MonoGCHandle (*pGchandleNew)(MonoObject*,int)=nullptr;
static MonoObject* (*pGchandleGet)(MonoGCHandle)=nullptr;
static void        (*pGchandleFree)(MonoGCHandle)=nullptr;
static MonoDomain* (*pGetRootDomain)(void)=nullptr;
static MonoDomain* (*pDomainGet)(void)=nullptr;

// ---- 全局状态 ----
static MonoDomain* gDomain=nullptr;
static void* gCorlib=nullptr;
static MonoClass* gClsOff=nullptr;
static MonoField gDictField=nullptr;

static std::atomic<bool> gSetup{false};
static std::atomic<bool> gTextHooked{false};
static std::atomic<bool> gSerHooked{false};
static std::atomic<bool> gCmHooked{false};
static std::atomic<bool> gInSetup{false};

static MonoGCHandle gGoodHandle=0;
static std::map<int, MonoGCHandle> gGoodMap;
static std::mutex gMapMutex;
static std::unordered_map<std::string,std::string> gDict;

#define FULL 1000
static const int SER_CAP = 40;
static int serCount=0; static bool serDone=false;

// ---- 已解析的托管方法指针 ----
typedef MonoString* (*fnGetText)(void*);
typedef float      (*fnGetSize)(MonoObject*);
typedef void       (*fnSetText)(void*,MonoString*);
typedef void       (*fnSetFont)(void*,MonoObject*);
typedef void       (*fnMusicPlay)(void*);
typedef MonoString*(*fnLdstr)(void*,void*,void*,void*);
typedef void       (*fnSerialize)(void*,void*,void*,void*);
typedef MonoObject*(*fnLoad3)(void*,void*,void*,void*);
typedef MonoObject*(*fnLoad2)(void*,void*,void*);
typedef int        (*fnJitExec)(MonoDomain*,void*,int,char**);
typedef MonoDomain*(*fnJitInit)(const char*);
typedef int        (*fnRunMain)(void*,int,char**);

static fnGetText   getTextFn=nullptr;
static fnGetSize   getSizeFn=nullptr;
static fnSetText   origSetText=nullptr;
static fnSetFont   origSetFont=nullptr;
static fnLdstr     origLdstr=nullptr;
static fnSerialize origSerialize=nullptr;
static fnLoad3     origLoad3=nullptr;
static fnLoad2     origLoad2=nullptr;
static fnJitExec   origJitExec=nullptr;
static fnJitInit   origJitInit=nullptr;
static fnRunMain   origRunMain=nullptr;
static fnMusicPlay origMusicPlay=nullptr;

// 码率: 头显把归一化档位(Limit)发给 PC, PC 用自身 min/max 表算实际码率
typedef int32_t (*fnCalcBitrate2)(int32_t,float);                    // GetDesktopBitrate(HmdType,float)
typedef int32_t (*fnCalcBitrate4)(int32_t,float,int32_t,int32_t);    // GetVRBitrate(HmdType,float,VideoCodec,bool)
typedef MonoString* (*fnBitrateStr2)(int32_t,float);                 // GetDesktopBitrateString(HmdType,float)
typedef MonoString* (*fnBitrateStr4)(int32_t,float,int32_t,int32_t); // GetVRBitrateString(...)
typedef void (*fnSetLimit1)(void*,float);                            // set_DesktopBitrateLimit / set_VRBitrateLimit
static fnCalcBitrate2 origDesktopBitrate=nullptr;
static fnCalcBitrate4 origVRBitrate=nullptr;
static fnBitrateStr2  origDesktopBitrateString=nullptr;
static fnBitrateStr4  origVRBitrateString=nullptr;
static fnSetLimit1    origSetDesktopLimit=nullptr;
static fnSetLimit1    origSetVRLimit=nullptr;
typedef void (*fnLimitChangedUI)(void*,void*,void*);  // SettingsTab/StreamingTab 的 On*BitrateLimitChanged(UI 同步)
typedef void (*fnSetLimit)(void*,float);              // LimitSlider::set_Limit
static fnLimitChangedUI origSettingsDesktopLimitChanged=nullptr;
static fnLimitChangedUI origStreamingVRLimitChanged=nullptr;
static fnSetLimit       origLimitSliderSetLimit=nullptr;

// 精确倍率补偿所需: min/max + 启动时捕获的 HmdType/VideoCodec/Foveated
typedef int32_t (*fnMinMax1)(int32_t);
typedef int32_t (*fnMinMax3)(int32_t,int32_t,int32_t);
static fnMinMax1 fnMinDesk=nullptr, fnMaxDesk=nullptr, fnMinVR=nullptr;
static fnMinMax3 fnMaxVR=nullptr;
static std::atomic<int32_t> gHmd{-1}, gCodec{-1}, gFov{-1};
typedef void (*fnSetInt1)(void*,int32_t);
static fnSetInt1 origSetHmdType=nullptr, origSetActiveCodec=nullptr, origSetFoveated=nullptr;
static fnSetInt1 origSetMeasuredBandwidth=nullptr;
static void (*origSendReload)(void*)=nullptr;
static std::atomic<bool> gSpeedtestDone{false};   // 测速完成前不下发放大值(避免连接期改动导致失败)

// 整包序列化(发送设置到 PC)钩子
typedef void (*fnSerializerObject)(void*,void*);
static fnSerializerObject origSerializerObject=nullptr;
static MonoClass* gClsUserSettings=nullptr;
static void* gUserSettingsPin=nullptr;                 // SharedUserSettings 实例(固定), 用于测速后补发档位
static void (*origUserSendReload)(void*)=nullptr;
static MonoField  gFieldDesktopLimit=nullptr;
static MonoField  gFieldVRLimit=nullptr;

static std::atomic<bool> gNoMusic{false};
static std::atomic<bool> gMusicHooked{false};
static std::atomic<bool> gMusicTried{false};

static std::atomic<bool> gQualitySound{false};   // 性能提示音总开关
static std::atomic<int>  gQualityMask{0x7F};     // 7 个提示音各自的位
static std::atomic<bool> gQualitySoundTried{false};

static std::atomic<float> gBitrateScale{2.0f};        // 码率倍率
static std::atomic<bool>  gBitrateExtend{false};      // 扩展码率范围
static std::atomic<bool>  gBitrateUnlock{false};      // 解除自动测速码率限制
static std::atomic<bool>  gBitrateHooked{false};
static std::atomic<bool>  gBitrateTried{false};

// ---- UTF 工具 ----
static void utf8ToUtf16(const std::string& u8, std::vector<uint16_t>& out){
  out.clear(); out.reserve(u8.size());
  size_t i=0, n=u8.size();
  while(i<n){
    unsigned char c=(unsigned char)u8[i];
    uint32_t cp=0; int extra=0;
    if(c<0x80){ cp=c; extra=0; }
    else if((c&0xE0)==0xC0){ cp=c&0x1F; extra=1; }
    else if((c&0xF0)==0xE0){ cp=c&0x0F; extra=2; }
    else if((c&0xF8)==0xF0){ cp=c&0x07; extra=3; }
    else { i++; continue; }
    if(i+(size_t)extra>=n) break;
    for(int k=0;k<extra;k++) cp=(cp<<6)|(u8[i+1+k]&0x3F);
    i+=1+extra;
    if(cp<=0xFFFF) out.push_back((uint16_t)cp);
    else { cp-=0x10000; out.push_back((uint16_t)(0xD800|(cp>>10))); out.push_back((uint16_t)(0xDC00|(cp&0x3FF))); }
  }
}
static std::string utf16ToUtf8(const uint16_t* s,int len){
  std::string o; o.reserve((size_t)len*3);
  for(int i=0;i<len;i++){
    uint32_t cp=s[i];
    if(cp>=0xD800 && cp<=0xDBFF && i+1<len && s[i+1]>=0xDC00 && s[i+1]<=0xDFFF){
      cp=0x10000+((cp-0xD800)<<10)+(s[i+1]-0xDC00); i++;
    }
    if(cp<0x80) o+=(char)cp;
    else if(cp<0x800){ o+=(char)(0xC0|(cp>>6)); o+=(char)(0x80|(cp&0x3F)); }
    else if(cp<0x10000){ o+=(char)(0xE0|(cp>>12)); o+=(char)(0x80|((cp>>6)&0x3F)); o+=(char)(0x80|(cp&0x3F)); }
    else { o+=(char)(0xF0|(cp>>18)); o+=(char)(0x80|((cp>>12)&0x3F)); o+=(char)(0x80|((cp>>6)&0x3F)); o+=(char)(0x80|(cp&0x3F)); }
  }
  return o;
}
static bool hasCjk(const uint16_t* s,int len){ for(int i=0;i<len;i++) if(s[i]>0x2E7F) return true; return false; }
static int  monoStrLen(MonoString* s){ return s?*(int32_t*)((char*)s+16):0; }
static const uint16_t* monoStrChars(MonoString* s){ return (const uint16_t*)((char*)s+20); }
static int rnd(float f){ return (int)(f>=0.0f?f+0.5f:f-0.5f); }

static MonoDomain* getDomain(){
  if(pDomainGet){ MonoDomain* d=pDomainGet(); if(d) return d; }
  if(gDomain) return gDomain;
  if(pGetRootDomain) return pGetRootDomain();
  return nullptr;
}

static MonoString* makeMono(const std::string& u8){
  if(!pStringNewUtf16) return nullptr;
  static thread_local std::vector<uint16_t> w;
  utf8ToUtf16(u8,w);
  if(w.empty()) return nullptr;
  return pStringNewUtf16(getDomain(), w.data(), (int32_t)w.size());
}

// ---- 类/方法解析 ----
static MonoClass* getClass(const char* qname){
  if(!pClassFromName||!pClassGetMethod||!pRuntimeInvoke||!pTypeGetClass||!gCorlib) return nullptr;
  MonoClass* ct=pClassFromName((MonoImage*)gCorlib,"System","Type");
  if(!ct) return nullptr;
  MonoMethod* mGT=pClassGetMethod(ct,"GetType",1);
  if(!mGT) return nullptr;
  MonoString* nm=makeMono(qname);
  if(!nm) return nullptr;
  void* a[1]; a[0]=nm;
  MonoObject* exc=nullptr;
  MonoObject* t=pRuntimeInvoke(mGT,nullptr,a,&exc);
  if(!t||exc) return nullptr;
  void* typePtr=*(void**)((char*)t+16);
  if(!typePtr) return nullptr;
  return pTypeGetClass((MonoType*)typePtr);
}
static void* methodAddr(MonoClass* c,const char* n,int np){
  if(!c||!pClassGetMethod||!pCompileMethod) return nullptr;
  MonoMethod* m=pClassGetMethod(c,n,np);
  if(!m) return nullptr;
  return pCompileMethod(m);
}

// ---- 字体能力探测 ----
static int glyphCount(MonoObject* font){
  if(!font||!gDictField||!pFieldGetValue||!pObjGetClass||!pClassGetMethod||!pRuntimeInvoke||!pObjUnbox) return -1;
  void* dict=nullptr;
  pFieldGetValue(font,gDictField,&dict);
  MonoObject* d=(MonoObject*)dict;
  if(!d) return 0;
  MonoClass* dc=pObjGetClass(d);
  if(!dc) return -1;
  MonoMethod* cm=pClassGetMethod(dc,"get_Count",0);
  if(!cm) return -1;
  MonoObject* exc=nullptr;
  MonoObject* r=pRuntimeInvoke(cm,d,nullptr,&exc);
  if(exc||!r) return -1;
  int* pi=(int*)pObjUnbox(r);
  return pi?*pi:-1;
}
static float fsize(MonoObject* f){ return (f&&getSizeFn)?getSizeFn(f):0.0f; }
static bool isFontClass(MonoObject* o){
  if(!o||!pObjGetClass||!pClassGetParent||!gClsOff) return false;
  MonoClass* c=pObjGetClass(o);
  MonoClass* cur=c;
  for(int i=0;i<8 && cur;i++){
    if(cur==gClsOff) return true;
    cur=pClassGetParent(cur);
  }
  return false;
}

// ---- goodFont/goodMap(pinned gchandle 防移动GC) ----
static MonoGCHandle pin(MonoObject* o){ return (o&&pGchandleNew)?pGchandleNew(o,1):0; }
static MonoObject* mov(MonoGCHandle h){ return (h&&pGchandleGet)?pGchandleGet(h):nullptr; }
static void markGood(MonoObject* f,const char* tag){
  if(!f) return;
  int g=glyphCount(f);
  if(g<FULL) return;
  float sz=fsize(f);
  int k=rnd(sz);
  if(!gGoodHandle){ gGoodHandle=pin(f); LOGI("[F] goodFont(%s) size=%.1f glyphs=%d",tag,sz,g); }
  std::lock_guard<std::mutex> lk(gMapMutex);
  if(gGoodMap.find(k)==gGoodMap.end()) gGoodMap[k]=pin(f);
}
static bool goodMapHas(int k){ std::lock_guard<std::mutex> lk(gMapMutex); return gGoodMap.find(k)!=gGoodMap.end(); }
// 选择替换字体: 30/32 号就近匹配; 其他字号优先用非 32 号中最大者, 没有则退回 32 号
static MonoObject* pickReplacement(float size){
  MonoGCHandle h=0;
  {
    std::lock_guard<std::mutex> lk(gMapMutex);
    if(!gGoodHandle) return nullptr;
    int r=rnd(size);
    if(r==30 || r==32){
      auto it=gGoodMap.find(r);
      if(it!=gGoodMap.end()){ h=it->second; }
      else {
        MonoGCHandle best=gGoodHandle; int bd=1<<30;
        for(auto& kv:gGoodMap){ int d=abs(kv.first-r); if(d<bd){ bd=d; best=kv.second; } }
        h=best;
      }
    } else {
      int mk=-1000000;
      for(auto& kv:gGoodMap){ if(kv.first!=32 && kv.first>mk){ mk=kv.first; h=kv.second; } }
      if(!h){
        auto it2=gGoodMap.find(32);
        h=(it2!=gGoodMap.end())?it2->second:gGoodHandle;
      }
    }
  }
  return h?mov(h):nullptr;
}

// ---- set_Text hook: 仅做翻译; 字体处理全部交给 set_Font(避免 setter 内重入写 Font) ----
static void hookSetText(void* tb,MonoString* text){
  int len=monoStrLen(text);
  if(len<=0 || len>2048){ if(origSetText) origSetText(tb,text); return; }
  std::string key=utf16ToUtf8(monoStrChars(text),len);
  auto it=gDict.find(key);
  if(it!=gDict.end()){
    MonoString* zh=makeMono(it->second);
    if(zh){
      text=zh;
    }
  }
  if(origSetText) origSetText(tb,text);
}

// ---- set_Font hook: 对含中文文本近似匹配最近字号全字体 ----
static void hookSetFont(void* tb,MonoObject* newFont){
  MonoObject* cur=newFont;
  MonoObject* gf=mov(gGoodHandle);
  if(gf && cur && cur!=gf){
    bool cjk=false;
    if(getTextFn){ MonoString* p=getTextFn(tb); cjk=hasCjk(monoStrChars(p),monoStrLen(p)); }
    if(cjk){
      int gc=glyphCount(cur);
      if(gc<FULL){
        float oS=fsize(cur);
        MonoObject* chN=pickReplacement(oS);
        if(chN && chN!=cur) cur=chN;
      }
    }
  }
  if(origSetFont) origSetFont(tb,cur);
}

// ---- MusicSystem::Play hook: 禁用后台自动播放音乐(唯一播放入口) ----
static void hookMusicPlay(void* self){
  if(gNoMusic.load()) return;
  if(origMusicPlay) origMusicPlay(self);
}

// ---- 性能提示音: 按 gQualityMask 位掩码逐个静音 ----
static void* gOrigSound[7]={nullptr};
template<int B> static void hookMuteSound(void* self){
  if(gQualitySound.load() && (gQualityMask.load()&(1<<B))) return;
  if(gOrigSound[B]) ((void(*)(void*))gOrigSound[B])(self);
}
static bool installQualitySoundHooks(){
  MonoClass* ss=getClass("VirtualDesktop.Mobile.SoundSystem, VirtualDesktop.Mobile");
  if(!ss){ LOGE("SoundSystem class not found"); return false; }
  static const char* names[7]={
    "PlayPotato","PlayUltra","PlayGodlike","PlayMonster",
    "PlayRampage","PlayUnstoppable","PlayHolyShit"
  };
  int ok=0;
  void* a0=methodAddr(ss,names[0],0); if(a0&&hook(a0,(void*)hookMuteSound<0>,&gOrigSound[0])==0) ok++;
  void* a1=methodAddr(ss,names[1],0); if(a1&&hook(a1,(void*)hookMuteSound<1>,&gOrigSound[1])==0) ok++;
  void* a2=methodAddr(ss,names[2],0); if(a2&&hook(a2,(void*)hookMuteSound<2>,&gOrigSound[2])==0) ok++;
  void* a3=methodAddr(ss,names[3],0); if(a3&&hook(a3,(void*)hookMuteSound<3>,&gOrigSound[3])==0) ok++;
  void* a4=methodAddr(ss,names[4],0); if(a4&&hook(a4,(void*)hookMuteSound<4>,&gOrigSound[4])==0) ok++;
  void* a5=methodAddr(ss,names[5],0); if(a5&&hook(a5,(void*)hookMuteSound<5>,&gOrigSound[5])==0) ok++;
  void* a6=methodAddr(ss,names[6],0); if(a6&&hook(a6,(void*)hookMuteSound<6>,&gOrigSound[6])==0) ok++;
  LOGI("hooked quality sounds: %d/7 mask=%d",ok,gQualityMask.load());
  return true;
}

// ================= 码率解锁 =================
// PC 按 bps=min+(max-min)*t 计算; 下发档位反解为把目标范围放大 s 倍后的档位, 头显本地保留原值
static float scaleFrac(int32_t mn,int32_t mx,float t,float s){
  if(mx<=mn || s<=1.0f) return t;
  double k=((double)mx*(double)s-(double)mn)/((double)mx-(double)mn);
  return (float)((double)t*k);
}
static float compensateDesktop(int32_t hmd,float t){
  if(t<0.0f) t=0.0f; else if(t>1.0f) t=1.0f;   // 档位恒在 [0,1]
  if(!gBitrateExtend.load() || !gSpeedtestDone.load()) return t;   // 测速前保持原值
  float s=gBitrateScale.load();
  if(s<=1.0f) return t;
  if(hmd<0 || !fnMinDesk || !fnMaxDesk) return t*s;   // 上下文未知: 退化为直接缩放
  return scaleFrac(fnMinDesk(hmd),fnMaxDesk(hmd),t,s);
}
static float compensateVR(int32_t hmd,float t,int32_t codec,int32_t fov){
  if(t<0.0f) t=0.0f; else if(t>1.0f) t=1.0f;   // 档位恒在 [0,1]
  if(!gBitrateExtend.load() || !gSpeedtestDone.load()) return t;   // 测速前保持原值
  float s=gBitrateScale.load();
  if(s<=1.0f) return t;
  if(hmd<0 || codec<0 || !fnMinVR || !fnMaxVR) return t*s;
  return scaleFrac(fnMinVR(hmd),fnMaxVR(hmd,codec,fov),t,s);
}
// ---- 启动时捕获补偿所需上下文 ----
static void hookCaptureHmd(void* self,int32_t v){ gHmd.store(v); if(origSetHmdType) origSetHmdType(self,v); }
static void hookCaptureCodec(void* self,int32_t v){ gCodec.store(v); if(origSetActiveCodec) origSetActiveCodec(self,v); }
static void hookCaptureFov(void* self,int32_t v){ gFov.store(v); if(origSetFoveated) origSetFoveated(self,v); }

// ---- 头显本地 bps 放大(内部状态/性能浮层) ----
static thread_local bool tInSetDesktop=false, tInSetVR=false;   // setter 内档位已是补偿值
static thread_local bool tInStrDesktop=false, tInStrVR=false;   // 字符串 hook 内同理
static thread_local bool tDeskLimitCtx=false, tVRLimitCtx=false; // 正在计算软上限标记
static void (*origSettingsMeasured)(void*)=nullptr;
static void (*origUpdateVRLimit)(void*)=nullptr;
static void hookSettingsMeasured(void* self){ tDeskLimitCtx=true; if(origSettingsMeasured) origSettingsMeasured(self); tDeskLimitCtx=false; }
static void hookUpdateVRLimit(void* self){ tVRLimitCtx=true; if(origUpdateVRLimit) origUpdateVRLimit(self); tVRLimitCtx=false; }
static int32_t hookDesktopBitrate(int32_t hmd,float t){
  gHmd.store(hmd);
  if(!origDesktopBitrate) return 0;
  return (tInSetDesktop||tInStrDesktop) ? origDesktopBitrate(hmd,t)
                                        : origDesktopBitrate(hmd,compensateDesktop(hmd,t));
}
static int32_t hookVRBitrate(int32_t hmd,float t,int32_t codec,int32_t fov){
  gHmd.store(hmd); gCodec.store(codec); gFov.store(fov);
  if(!origVRBitrate) return 0;
  return (tInSetVR||tInStrVR) ? origVRBitrate(hmd,t,codec,fov)
                              : origVRBitrate(hmd,compensateVR(hmd,t,codec,fov),codec,fov);
}
// ---- 显示: 用补偿档位格式化(即使 GetDesktopBitrate 被内联也生效) ----
static MonoString* hookDesktopBitrateString(int32_t hmd,float t){
  gHmd.store(hmd);
  if(!origDesktopBitrateString) return nullptr;
  if(!gBitrateExtend.load()) return origDesktopBitrateString(hmd,t);
  tInStrDesktop=true;
  MonoString* r=origDesktopBitrateString(hmd,compensateDesktop(hmd,t));
  tInStrDesktop=false;
  return r;
}
static MonoString* hookVRBitrateString(int32_t hmd,float t,int32_t codec,int32_t fov){
  gHmd.store(hmd); gCodec.store(codec); gFov.store(fov);
  if(!origVRBitrateString) return nullptr;
  if(!gBitrateExtend.load()) return origVRBitrateString(hmd,t,codec,fov);
  tInStrVR=true;
  MonoString* r=origVRBitrateString(hmd,compensateVR(hmd,t,codec,fov),codec,fov);
  tInStrVR=false;
  return r;
}

// ---- 发往 PC(增量属性变更): 放大档位后立即恢复头显本地原值 ----
static void hookSetDesktopLimit(void* self,float v){
  if(!origSetDesktopLimit) return;
  float s=gBitrateScale.load();
  if(v<0.0f) v=0.0f; else if(v>1.0f) v=1.0f;
  if(!gBitrateExtend.load() || s<=1.0f){ origSetDesktopLimit(self,v); return; }
  MonoGCHandle h=0; void* pin=self;
  if(pGchandleNew){ h=pGchandleNew((MonoObject*)self,1); if(pGchandleGet) pin=pGchandleGet(h); }
  float ts=compensateDesktop(gHmd.load(),v);
  tInSetDesktop=true; origSetDesktopLimit(pin,ts); tInSetDesktop=false;
  if(gFieldDesktopLimit && pFieldSetValue) pFieldSetValue(pin,gFieldDesktopLimit,&v);
  if(h && pGchandleFree) pGchandleFree(h);
}
static void hookSetVRLimit(void* self,float v){
  if(!origSetVRLimit) return;
  float s=gBitrateScale.load();
  if(v<0.0f) v=0.0f; else if(v>1.0f) v=1.0f;
  if(!gBitrateExtend.load() || s<=1.0f){ origSetVRLimit(self,v); return; }
  MonoGCHandle h=0; void* pin=self;
  if(pGchandleNew){ h=pGchandleNew((MonoObject*)self,1); if(pGchandleGet) pin=pGchandleGet(h); }
  float ts=compensateVR(gHmd.load(),v,gCodec.load(),gFov.load());
  tInSetVR=true; origSetVRLimit(pin,ts); tInSetVR=false;
  if(gFieldVRLimit && pFieldSetValue) pFieldSetValue(pin,gFieldVRLimit,&v);
  if(h && pGchandleFree) pGchandleFree(h);
}
// 设置变更事件会把滑块 Value 同步为"当前档位"; setter 期间跳过, 避免滑块被顶到补偿值
static void hookSettingsDesktopLimitChanged(void* self,void* sender,void* e){
  if(!tInSetDesktop && origSettingsDesktopLimitChanged) origSettingsDesktopLimitChanged(self,sender,e);
}
static void hookStreamingVRLimitChanged(void* self,void* sender,void* e){
  if(!tInSetVR && origStreamingVRLimitChanged) origStreamingVRLimitChanged(self,sender,e);
}
// ---- LimitSlider: unlock 时顶到 Maximum; extend 时把标记档位重映射到放大后的范围 ----
static void hookLimitSliderSetLimit(void* self,float v){
  if(gBitrateUnlock.load()){
    v=1e9f;
  } else if(gBitrateExtend.load() && gBitrateScale.load()>1.0f){
    int32_t mn=-1,mx=-1;
    if(tVRLimitCtx){ mn=fnMinVR?fnMinVR(gHmd.load()):-1; mx=(fnMaxVR&&gHmd.load()>=0&&gCodec.load()>=0)?fnMaxVR(gHmd.load(),gCodec.load(),gFov.load()):-1; }
    else if(tDeskLimitCtx){ mn=fnMinDesk?fnMinDesk(gHmd.load()):-1; mx=fnMaxDesk?fnMaxDesk(gHmd.load()):-1; }
    if(mn>=0 && mx>mn){
      double denom=(double)mx*(double)gBitrateScale.load()-(double)mn;
      v=(float)((double)v*((double)mx-(double)mn)/denom);
    }
  }
  if(origLimitSliderSetLimit) origLimitSliderSetLimit(self,v);
}
// ---- 解除测速上限: 直接把上报带宽顶到 1.5Gbps ----
static void hookSetMeasuredBandwidth(void* self,int32_t v){
  bool firstReal=(v>0 && !gSpeedtestDone.load());
  int32_t out=v;
  // 测速完成前原样放行(不改连接期参数); 完成后才顶到 1.5Gbps 解除上限
  if(gBitrateUnlock.load() && (gSpeedtestDone.load() || firstReal)) out=1500000000;
  if(firstReal) gSpeedtestDone.store(true);
  if(origSetMeasuredBandwidth) origSetMeasuredBandwidth(self,out);   // 内部按放大档位重算 bps
  // 测速完成后重新整包发送(连接时那份 bps 还是 0)
  if(firstReal){
    if(origSendReload) origSendReload(self);
    if(origUserSendReload && gUserSettingsPin) origUserSendReload(gUserSettingsPin);
  }
}
// ---- 发往 PC(整包): 连接时 SendSettingsReload 直接序列化字段, 绕过 setter ----
static void hookSerializerObject(void* obj,void* writer){
  if(!origSerializerObject){ return; }
  // 首次序列化 SharedUserSettings 时固定实例, 供测速后补发档位
  if(!gUserSettingsPin && obj && pObjGetClass && gClsUserSettings && pGchandleNew
     && pObjGetClass((MonoObject*)obj)==gClsUserSettings){
    MonoGCHandle gh=pGchandleNew((MonoObject*)obj,1);
    if(gh && pGchandleGet) gUserSettingsPin=pGchandleGet(gh);
  }
  float s=gBitrateScale.load();
  if(!gBitrateExtend.load() || s<=1.0f || !obj || !gClsUserSettings || !gFieldDesktopLimit || !gFieldVRLimit
     || !pFieldGetValue || !pFieldSetValue || !pObjGetClass
     || pObjGetClass((MonoObject*)obj)!=gClsUserSettings){
    origSerializerObject(obj,writer); return;
  }
  // 发往 PC 的档位用放大值, 头显本地保持原值
  MonoGCHandle h=0; void* pin=obj;
  if(pGchandleNew){ h=pGchandleNew((MonoObject*)obj,1); if(pGchandleGet) pin=pGchandleGet(h); }
  float d0=0,v0=0;
  pFieldGetValue(pin,gFieldDesktopLimit,&d0);
  pFieldGetValue(pin,gFieldVRLimit,&v0);
  float d1=compensateDesktop(gHmd.load(),d0), v1=compensateVR(gHmd.load(),v0,gCodec.load(),gFov.load());
  pFieldSetValue(pin,gFieldDesktopLimit,&d1);
  pFieldSetValue(pin,gFieldVRLimit,&v1);
  origSerializerObject(pin,writer);
  pFieldSetValue(pin,gFieldDesktopLimit,&d0);
  pFieldSetValue(pin,gFieldVRLimit,&v0);
  if(h && pGchandleFree) pGchandleFree(h);
}

static bool installBitrateHooks(){
  int ok=0;
  MonoClass* hx=getClass("Xenko.VR.HmdResolutionTypeExtensions, VirtualDesktop.Mobile.Shared");
  if(hx){
    fnMinDesk=(fnMinMax1)methodAddr(hx,"GetMinDesktopBitrate",1);
    fnMaxDesk=(fnMinMax1)methodAddr(hx,"GetMaxDesktopBitrate",1);
    fnMinVR =(fnMinMax1)methodAddr(hx,"GetMinVRBitrate",1);
    fnMaxVR =(fnMinMax3)methodAddr(hx,"GetMaxVRBitrate",3);
    void* a;
    a=methodAddr(hx,"GetDesktopBitrate",2);       if(a && hook(a,(void*)hookDesktopBitrate,(void**)&origDesktopBitrate)==0) ok++;
    a=methodAddr(hx,"GetVRBitrate",4);            if(a && hook(a,(void*)hookVRBitrate,(void**)&origVRBitrate)==0) ok++;
    a=methodAddr(hx,"GetDesktopBitrateString",2); if(a && hook(a,(void*)hookDesktopBitrateString,(void**)&origDesktopBitrateString)==0) ok++;
    a=methodAddr(hx,"GetVRBitrateString",4);      if(a && hook(a,(void*)hookVRBitrateString,(void**)&origVRBitrateString)==0) ok++;
  } else LOGE("HmdResolutionTypeExtensions class not found");

  MonoClass* us=getClass("VirtualDesktop.Mobile.SharedUserSettings, VirtualDesktop.Mobile.Shared");
  if(us){
    if(pClassGetField){
      gFieldDesktopLimit=pClassGetField(us,"_desktopBitrateLimit");
      gFieldVRLimit=pClassGetField(us,"_vrBitrateLimit");
    }
    void* a;
    a=methodAddr(us,"set_DesktopBitrateLimit",1); if(a && hook(a,(void*)hookSetDesktopLimit,(void**)&origSetDesktopLimit)==0) ok++;
    a=methodAddr(us,"set_VRBitrateLimit",1);      if(a && hook(a,(void*)hookSetVRLimit,(void**)&origSetVRLimit)==0) ok++;
  } else LOGE("SharedUserSettings class not found");
  gClsUserSettings=us;
  if(us && pClassGetParent){   // 与 SharedMobileSettings 同一泛型基类, 沿父类链找 SendSettingsReload
    MonoClass* c=pClassGetParent(us);
    for(int i=0;i<4 && c && !origUserSendReload;i++){
      origUserSendReload=(void(*)(void*))methodAddr(c,"SendSettingsReload",0);
      if(!origUserSendReload) c=pClassGetParent(c);
    }
  }

  MonoClass* ms=getClass("VirtualDesktop.Mobile.SharedMobileSettings, VirtualDesktop.Mobile.Shared");
  if(ms){
    void* a;
    a=methodAddr(ms,"set_HmdType",1);            if(a && hook(a,(void*)hookCaptureHmd,(void**)&origSetHmdType)==0) ok++;
    a=methodAddr(ms,"set_FoveatedStreaming",1);   if(a && hook(a,(void*)hookCaptureFov,(void**)&origSetFoveated)==0) ok++;
    a=methodAddr(ms,"set_MeasuredBandwidth",1);   if(a && hook(a,(void*)hookSetMeasuredBandwidth,(void**)&origSetMeasuredBandwidth)==0) ok++;
    origSendReload=(void(*)(void*))methodAddr(ms,"SendSettingsReload",0);
    if(!origSendReload && pClassGetParent){   // 方法定义在泛型基类上: 沿父类链查找
      MonoClass* c=pClassGetParent(ms);
      for(int i=0;i<4 && c && !origSendReload;i++){
        origSendReload=(void(*)(void*))methodAddr(c,"SendSettingsReload",0);
        if(!origSendReload) c=pClassGetParent(c);
      }
    }
  } else LOGE("SharedMobileSettings class not found");
  MonoClass* ss=getClass("VirtualDesktop.Mobile.SharedStreamerSettings, VirtualDesktop.Mobile.Shared");
  if(ss){
    void* a=methodAddr(ss,"set_ActiveCodec",1);
    if(a && hook(a,(void*)hookCaptureCodec,(void**)&origSetActiveCodec)==0) ok++;
  } else LOGE("SharedStreamerSettings class not found");

  MonoClass* jh=getClass("VirtualDesktop.Core.JsonHelper, VirtualDesktop.Core");
  if(jh){
    void* a=methodAddr(jh,"SerializerObject",2);
    if(a && hook(a,(void*)hookSerializerObject,(void**)&origSerializerObject)==0) ok++;
  } else LOGE("JsonHelper class not found");

  MonoClass* st=getClass("VirtualDesktop.Mobile.SettingsTab, VirtualDesktop.Mobile");
  if(st){
    void* a=methodAddr(st,"OnDesktopBitrateLimitChanged",2);
    if(a && hook(a,(void*)hookSettingsDesktopLimitChanged,(void**)&origSettingsDesktopLimitChanged)==0) ok++;
    a=methodAddr(st,"OnMeasuredBandwidthChanged",0);
    if(a && hook(a,(void*)hookSettingsMeasured,(void**)&origSettingsMeasured)==0) ok++;
  } else LOGE("SettingsTab class not found");
  MonoClass* strm=getClass("VirtualDesktop.Mobile.StreamingTab, VirtualDesktop.Mobile");
  if(strm){
    void* a=methodAddr(strm,"OnVRBitrateLimitChanged",2);
    if(a && hook(a,(void*)hookStreamingVRLimitChanged,(void**)&origStreamingVRLimitChanged)==0) ok++;
    a=methodAddr(strm,"UpdateVRBitrateLimit",0);
    if(a && hook(a,(void*)hookUpdateVRLimit,(void**)&origUpdateVRLimit)==0) ok++;
  } else LOGE("StreamingTab class not found");
  MonoClass* ls=getClass("Xenko.UI.Controls.LimitSlider, Xenko.UI");
  if(ls){
    void* a=methodAddr(ls,"set_Limit",1);
    if(a && hook(a,(void*)hookLimitSliderSetLimit,(void**)&origLimitSliderSetLimit)==0) ok++;
  } else LOGE("LimitSlider class not found");

  gBitrateHooked.store(ok>0);
  LOGI("bitrate hooks=%d",ok);
  return hx && us;
}

// ---- Serialize hook: 载入监控捕获 goodFont/goodMap ----
static void hookSerialize(void* x0,void* x1,void* x2,void* x3){
  void* slot=x1;
  if(origSerialize) origSerialize(x0,x1,x2,x3);
  if(serDone) return;
  if(serCount>=SER_CAP){ serDone=true; return; }
  serCount++;
  if(!slot) return;
  MonoObject* o=*(MonoObject**)slot;
  if(!o || !isFontClass(o)) return;
  int g=glyphCount(o);
  if(g>=FULL){
    markGood(o,"ser");
    if(goodMapHas(14)&&goodMapHas(16)&&goodMapHas(20)&&goodMapHas(24)) serDone=true;
  }
}

// ---- ContentManager::Load hook: 载入监控捕获返回字体 ----
static void tryCaptLoad(MonoObject* o){
  if(!o || !isFontClass(o)) return;
  if(glyphCount(o)>=FULL) markGood(o,"load");
}
static MonoObject* hookLoad3(void* x0,void* x1,void* x2,void* x3){
  MonoObject* r=origLoad3?origLoad3(x0,x1,x2,x3):nullptr;
  tryCaptLoad(r);
  return r;
}
static MonoObject* hookLoad2(void* x0,void* x1,void* x2){
  MonoObject* r=origLoad2?origLoad2(x0,x1,x2):nullptr;
  tryCaptLoad(r);
  return r;
}

// ---- doSetup(只在托管线程执行) ----
static void doSetupImpl(){
  if(!gCorlib && pGetCorlib) gCorlib=pGetCorlib();
  if(!gCorlib) return;

  // 与字体无关的钩子尽早安装(不被字体块的提前 return 挡住); 类解析失败则下次再试
  if(!gQualitySoundTried.load() && installQualitySoundHooks()) gQualitySoundTried.store(true);
  if(!gBitrateTried.load() && installBitrateHooks()) gBitrateTried.store(true);
  if(!gMusicHooked.load() && !gMusicTried.load()){
    gMusicTried.store(true);
    MonoClass* ms=getClass("VirtualDesktop.Mobile.MusicSystem, VirtualDesktop.Mobile");
    if(ms){
      void* aPlay=methodAddr(ms,"Play",0);
      if(aPlay && hook(aPlay,(void*)hookMusicPlay,(void**)&origMusicPlay)==0){
        gMusicHooked.store(true);
        LOGI("hooked MusicSystem::Play");
      } else LOGE("MusicSystem::Play hook failed");
    } else LOGE("MusicSystem class not found");
  }

  if(!gTextHooked.load()){
    MonoClass* tb=getClass("Xenko.UI.Controls.TextBlock, Xenko.UI");
    if(!tb) return;
    void* aSet =methodAddr(tb,"set_Text",1);
    void* aSetF=methodAddr(tb,"set_Font",1);
    void* aGetT=methodAddr(tb,"get_Text",0);
    if(!aSet||!aSetF) return;
    getTextFn=(fnGetText)aGetT;
    if(hook(aSet,(void*)hookSetText,(void**)&origSetText)!=0) LOGE("set_Text hook failed");
    if(hook(aSetF,(void*)hookSetFont,(void**)&origSetFont)!=0) LOGE("set_Font hook failed");
    MonoClass* sf=getClass("Xenko.Graphics.SpriteFont, Xenko.Graphics");
    if(sf){ getSizeFn=(fnGetSize)methodAddr(sf,"get_Size",0); }
    MonoClass* off=getClass("Xenko.Graphics.Font.OfflineRasterizedSpriteFont, Xenko.Graphics");
    if(off&&pClassGetField){ gClsOff=off; gDictField=pClassGetField(off,"CharacterToGlyph"); }
    gTextHooked.store(true);
    LOGI("hooked set_Text + set_Font");
  }

  if(!gSerHooked.load()){
    MonoClass* ser=getClass("Xenko.Graphics.Font.OfflineRasterizedSpriteFontSerializer, Xenko.Graphics");
    if(ser){
      void* aSer=methodAddr(ser,"Serialize",3);
      if(aSer && hook(aSer,(void*)hookSerialize,(void**)&origSerialize)==0){
        gSerHooked.store(true);
        LOGI("hooked OfflineRasterizedSpriteFontSerializer::Serialize");
      }
    }
  }

  if(!gCmHooked.load()){
    MonoClass* cm=getClass("Xenko.Core.Serialization.Contents.ContentManager, Xenko.Core.Serialization");
    if(cm){
      void* aLoad3=methodAddr(cm,"Load",3);
      void* aLoad2=methodAddr(cm,"Load",2);
      if(aLoad3&&aLoad2 &&
         hook(aLoad3,(void*)hookLoad3,(void**)&origLoad3)==0 &&
         hook(aLoad2,(void*)hookLoad2,(void**)&origLoad2)==0){
        gCmHooked.store(true);
        LOGI("hooked ContentManager::Load");
      }
    }
  }

  if(gTextHooked.load()&&gSerHooked.load()&&gCmHooked.load()){ gSetup.store(true); LOGI("setup done"); }
}
static void doSetup(){
  if(gSetup.load()) return;
  bool expected=false;
  if(!gInSetup.compare_exchange_strong(expected,true)) return;  // 防重入(GetType 内部会再触发 ldstr)
  doSetupImpl();
  gInSetup.store(false);
}

// ---- 触发/domain 捕获 hooks(均跑在托管线程) ----
static int gLd=0;
static MonoString* hookLdstr(void* a0,void* a1,void* a2,void* a3){
  if(!gDomain && a0) gDomain=(MonoDomain*)a0;
  gLd++;
  if(!gSetup.load() && gLd%20==2) doSetup();
  return origLdstr?origLdstr(a0,a1,a2,a3):nullptr;
}
static MonoDomain* hookJitInit(const char* name){
  MonoDomain* d=origJitInit?origJitInit(name):nullptr;
  if(!gDomain && d) gDomain=d;
  if(!gSetup.load()) doSetup();
  return d;
}
static int hookJitExec(MonoDomain* domain,void* asm_,int argc,char** argv){
  if(!gDomain && domain) gDomain=domain;
  if(!gSetup.load()) doSetup();
  return origJitExec?origJitExec(domain,asm_,argc,argv):-1;
}
static int hookRunMain(void* asm_,int argc,char** argv){
  if(!gDomain && pGetRootDomain) gDomain=pGetRootDomain();
  if(!gSetup.load()) doSetup();
  return origRunMain?origRunMain(asm_,argc,argv):-1;
}

// ---- 解析 mono 符号(来自 on_library_loaded 的句柄) ----
static void resolveMono(void* mh){
  *(void**)&pGetCorlib       =dlsym(mh,"mono_get_corlib");
  *(void**)&pClassFromName   =dlsym(mh,"mono_class_from_name");
  *(void**)&pClassGetMethod  =dlsym(mh,"mono_class_get_method_from_name");
  *(void**)&pRuntimeInvoke   =dlsym(mh,"mono_runtime_invoke");
  *(void**)&pTypeGetClass    =dlsym(mh,"mono_type_get_class");
  *(void**)&pCompileMethod   =dlsym(mh,"mono_compile_method");
  *(void**)&pObjGetClass     =dlsym(mh,"mono_object_get_class");
  *(void**)&pObjUnbox        =dlsym(mh,"mono_object_unbox");
  *(void**)&pClassGetParent  =dlsym(mh,"mono_class_get_parent");
  *(void**)&pClassGetField   =dlsym(mh,"mono_class_get_field_from_name");
  *(void**)&pFieldGetValue   =dlsym(mh,"mono_field_get_value");
  *(void**)&pFieldSetValue   =dlsym(mh,"mono_field_set_value");
    *(void**)&pLdstrChecked    =dlsym(mh,"mono_ldstr_checked");
  *(void**)&pStringNewUtf16  =dlsym(mh,"mono_string_new_utf16");
  *(void**)&pGchandleNew     =dlsym(mh,"mono_gchandle_new");
  *(void**)&pGchandleGet     =dlsym(mh,"mono_gchandle_get_target");
  *(void**)&pGchandleFree    =dlsym(mh,"mono_gchandle_free");
  *(void**)&pGetRootDomain   =dlsym(mh,"mono_get_root_domain");
  *(void**)&pDomainGet       =dlsym(mh,"mono_domain_get");
}

// ---- LSPosed on_library_loaded 回调 ----
static void on_library_loaded(const char* name, void* handle){
  if(!name || !strstr(name,"libmonosgen-2.0.so")) return;
  resolveMono(handle);
  if(!pLdstrChecked){ LOGE("mono_ldstr_checked not found"); return; }
  if(hook((void*)pLdstrChecked,(void*)hookLdstr,(void**)&origLdstr)!=0) LOGE("ldstr hook failed");
  void* je=dlsym(handle,"mono_jit_exec");
  if(je) hook(je,(void*)hookJitExec,(void**)&origJitExec);
  void* ji=dlsym(handle,"mono_jit_init");
  if(ji) hook(ji,(void*)hookJitInit,(void**)&origJitInit);
  void* rm=dlsym(handle,"mono_runtime_run_main");
  if(rm) hook(rm,(void*)hookRunMain,(void**)&origRunMain);
  LOGI("mono hooked");
}

// ---- LSPosed native_init 入口 ----
extern "C" __attribute__((visibility("default"))) __attribute__((used))
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries){
  if(!entries || !entries->hook_func){ LOGE("invalid native entries"); return nullptr; }
  g_hook=entries->hook_func;
  return on_library_loaded;
}

// ---- JNI ----
extern "C" JNIEXPORT void JNICALL Java_org_ghitori_vdplus_MainModule_nativeInit(JNIEnv* env, jobject thiz){
  if(!g_hook) LOGE("native hook unavailable (native_init not called)");
}
extern "C" JNIEXPORT void JNICALL Java_org_ghitori_vdplus_MainModule_nativeSetNoMusic(JNIEnv* env, jobject thiz, jboolean disable){
  gNoMusic.store(disable==JNI_TRUE);
  LOGI("noMusic=%d",(int)gNoMusic.load());
}
extern "C" JNIEXPORT void JNICALL Java_org_ghitori_vdplus_MainModule_nativeSetQuality(JNIEnv* env, jobject thiz, jboolean enable, jint mask){
  gQualitySound.store(enable==JNI_TRUE);
  gQualityMask.store((int)mask & 0x7F);
  LOGI("quality enable=%d mask=%d",(int)gQualitySound.load(),gQualityMask.load());
}
extern "C" JNIEXPORT void JNICALL Java_org_ghitori_vdplus_MainModule_nativeSetBitrate(JNIEnv* env, jobject thiz, jboolean extend, jboolean unlock, jfloat scale){
  float s=scale;
  if(!(s>=0.0f)) s=0.0f;   // 同时处理 NaN
  if(s>5.0f) s=5.0f;
  gBitrateExtend.store(extend==JNI_TRUE);
  gBitrateUnlock.store(unlock==JNI_TRUE);
  gBitrateScale.store(s);
  LOGI("bitrate extend=%d unlock=%d scale=%.2f",(int)gBitrateExtend.load(),(int)gBitrateUnlock.load(),s);
}
extern "C" JNIEXPORT void JNICALL Java_org_ghitori_vdplus_MainModule_nativeSetDict(JNIEnv* env, jobject thiz, jobjectArray keys, jobjectArray vals){
  jint n=env->GetArrayLength(keys);
  jint m=env->GetArrayLength(vals);
  for(jint i=0;i<n && i<m;i++){
    jstring k=(jstring)env->GetObjectArrayElement(keys,i);
    jstring v=(jstring)env->GetObjectArrayElement(vals,i);
    const char* ck=k?env->GetStringUTFChars(k,nullptr):nullptr;
    const char* cv=v?env->GetStringUTFChars(v,nullptr):nullptr;
    if(ck&&cv) gDict[ck]=cv;
    if(ck) env->ReleaseStringUTFChars(k,ck);
    if(cv) env->ReleaseStringUTFChars(v,cv);
    if(k) env->DeleteLocalRef(k);
    if(v) env->DeleteLocalRef(v);
  }
  LOGI("dict=%d",(int)gDict.size());
}
