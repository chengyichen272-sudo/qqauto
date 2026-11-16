// auto_qqspeed.cpp — 极速领域(com.garena.game.fctw) 自动跑模块
//
// 设计取舍：全部走「运行时反射」，不硬编码任何方法地址 / 字段偏移。
// 这样游戏每次更新（如 1.55.0.27410 → 新版）都不需要重新逆向，
// 只要类名/方法名不变就能自动重新定位；名字变了也只需改下面的字符串常量。
//
// 与原始 libqc.so 的差别：
//   - 原始版本从 global-metadata 之外的路径硬编码了一部分流程，且体积 23MB（内嵌 UI 图）
//   - 本模块 < 100KB，无 UI，只做「录制 / 回放 / 按键注入」三件事
//
// 注入后自动执行：等 il2cpp 就绪 → 定位类 → 起节拍线程 → 按脚本驱动按键。
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include "il2cpp_api.h"

#define TAG "qqauto"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace ic {

Api g_ic{};

template <typename T>
static bool bind(void* h, const char* name, T& fn) {
    fn = reinterpret_cast<T>(dlsym(h, name));
    if (!fn) LOGE("dlsym miss: %s", name);
    return fn != nullptr;
}

bool Load(const char* so_name) {
    // 游戏已把 libil2cpp 映射进来，用 RTLD_NOLOAD 拿句柄，不会重复加载
    void* h = dlopen(so_name, RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen(so_name, RTLD_NOW);
    if (!h) {
        LOGE("dlopen %s failed: %s", so_name, dlerror());
        return false;
    }
    bool ok = true;
    ok &= bind(h, "il2cpp_domain_get", g_ic.domain_get);
    ok &= bind(h, "il2cpp_domain_get_assemblies", g_ic.domain_get_assemblies);
    ok &= bind(h, "il2cpp_assembly_get_image", g_ic.assembly_get_image);
    ok &= bind(h, "il2cpp_class_from_name", g_ic.class_from_name);
    ok &= bind(h, "il2cpp_image_get_class_count", g_ic.image_get_class_count);
    ok &= bind(h, "il2cpp_image_get_class", g_ic.image_get_class);
    ok &= bind(h, "il2cpp_class_get_method_from_name", g_ic.class_get_method_from_name);
    ok &= bind(h, "il2cpp_class_get_field_from_name", g_ic.class_get_field_from_name);
    ok &= bind(h, "il2cpp_field_get_offset", g_ic.field_get_offset);
    ok &= bind(h, "il2cpp_method_get_name", g_ic.method_get_name);
    ok &= bind(h, "il2cpp_class_get_name", g_ic.class_get_name);
    ok &= bind(h, "il2cpp_class_get_namespace", g_ic.class_get_namespace);
    ok &= bind(h, "il2cpp_method_get_param_count", g_ic.method_get_param_count);
    ok &= bind(h, "il2cpp_string_new", g_ic.string_new);
    ok &= bind(h, "il2cpp_thread_attach", g_ic.thread_attach);
    ok &= bind(h, "il2cpp_thread_detach", g_ic.thread_detach);
    ok &= bind(h, "il2cpp_class_get_type", g_ic.class_get_type);
    ok &= bind(h, "il2cpp_type_get_object", g_ic.type_get_object);
    ok &= bind(h, "il2cpp_runtime_invoke", g_ic.runtime_invoke);
    ok &= bind(h, "il2cpp_field_static_get_value", g_ic.field_static_get_value);
    ok &= bind(h, "il2cpp_gchandle_new", g_ic.gchandle_new);
    ok &= bind(h, "il2cpp_gchandle_get_target", g_ic.gchandle_get_target);
    LOGI("il2cpp symbols bound: %s", ok ? "ok" : "partial");
    return ok;
}

}  // namespace ic

// ---------------------------------------------------------------- 类查找

static Il2CppClass* find_class_by_name(const char* name, const char* ns_hint = nullptr) {
    auto* dom = ic::g_ic.domain_get();
    if (!dom) return nullptr;
    size_t n = 0;
    const Il2CppAssembly** asms = ic::g_ic.domain_get_assemblies(dom, &n);
    for (size_t i = 0; i < n; i++) {
        const Il2CppImage* img = ic::g_ic.assembly_get_image(asms[i]);
        if (!img) continue;
        if (ns_hint) {
            if (auto* c = ic::g_ic.class_from_name(img, ns_hint, name)) {
                LOGI("class %s::%s found in image %zu", ns_hint, name, i);
                return c;
            }
        }
        // 不带命名空间先试一次，再全量扫
        if (auto* c = ic::g_ic.class_from_name(img, "", name)) {
            LOGI("class %s found in image %zu (ns=\"\")", name, i);
            return c;
        }
    }
    // 全量扫（慢，但只在启动时做一次）
    for (size_t i = 0; i < n; i++) {
        const Il2CppImage* img = ic::g_ic.assembly_get_image(asms[i]);
        size_t cc = ic::g_ic.image_get_class_count(img);
        for (size_t k = 0; k < cc; k++) {
            Il2CppClass* c = ic::g_ic.image_get_class(img, k);
            if (c && strcmp(ic::g_ic.class_get_name(c), name) == 0) {
                LOGI("class %s found by scan (image %zu, ns=%s)", name, i, ic::g_ic.class_get_namespace(c));
                return c;
            }
        }
    }
    LOGE("class %s not found", name);
    return nullptr;
}

static Il2CppClass* g_uo_cls = nullptr;

static Il2CppObject* find_scene_object(Il2CppClass* cls) {
    // UnityEngine.Object.FindObjectOfType(Type)   —— 类句柄缓存，避免每帧全量扫描
    if (!g_uo_cls) g_uo_cls = find_class_by_name("Object", "UnityEngine");
    Il2CppClass* uo = g_uo_cls;
    if (!uo) return nullptr;
    // argc 传 -1 表示不校验参数个数，拿到同名重载即可
    MethodInfo* mi = ic::g_ic.class_get_method_from_name(uo, "FindObjectOfType", -1);
    if (!mi) mi = ic::g_ic.class_get_method_from_name(uo, "FindObjectOfType", 1);
    if (!mi) { LOGE("FindObjectOfType not found"); return nullptr; }

    Il2CppType* t = ic::g_ic.class_get_type(cls);
    if (!t) { LOGE("class_get_type failed"); return nullptr; }
    Il2CppObject* sys_type = ic::g_ic.type_get_object(t);
    if (!sys_type) { LOGE("type_get_object failed"); return nullptr; }

    // 同时准备 1 参和 2 参两种重载的参数：runtime_invoke 按方法签名取用，多给的会被忽略
    static bool s_false = false;
    void* args[2] = {sys_type, &s_false};
    Il2CppObject* exc = nullptr;
    Il2CppObject* obj = ic::g_ic.runtime_invoke(mi, nullptr, args, &exc);
    if (exc) LOGE("FindObjectOfType threw");
    return obj;
}

// ---------------------------------------------------------------- 按键注入

// 按键位定义：不同版本可能不同，按实际游戏标定后改这里。
// 标定办法：用外部采集（回放画面里方向键/漂移键的高亮）反推「何时按下」，
//          再对照 mask 日志，即可确定每一位的含义。
enum : uint32_t {
    KEY_LEFT   = 1u << 0,
    KEY_RIGHT  = 1u << 1,
    KEY_DRIFT  = 1u << 2,
    KEY_NITRO  = 1u << 3,
    KEY_ITEM   = 1u << 4,
};

struct Ctrl {
    Il2CppClass* cls = nullptr;
    void* instance = nullptr;
    void* set_key_state = nullptr;      // SetKeyState(int keyType, bool down)
    void* set_key_flags = nullptr;      // SetKeyStateFlags(uint32 mask)
    void* match_key_type = nullptr;     // MatchInputKeyType(...)
    void* reset_all = nullptr;          // ResetAllKeyState()
    int argc_set_key_state = -1;
};

static Ctrl g_ctrl;
static volatile uint32_t g_mask = 0;
static volatile int g_running = 0;
static char g_replay_path[256] = "/sdcard/Android/data/com.garena.game.fctw/files/qqauto.replay";
static volatile int g_mode = 0;  // 0=idle 1=replay 2=record

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static bool resolve_vehicle_ctrl() {
    Il2CppClass* c = find_class_by_name("VehicleInputCtrl");
    if (!c) c = find_class_by_name("VehicleInputControl");
    if (!c) return false;
    g_ctrl.cls = c;

    if (MethodInfo* mi = ic::g_ic.class_get_method_from_name(c, "SetKeyState", 2))
        g_ctrl.set_key_state = ic::MethodPtr(mi);
    if (MethodInfo* mi = ic::g_ic.class_get_method_from_name(c, "SetKeyStateFlags", 1))
        g_ctrl.set_key_flags = ic::MethodPtr(mi);
    if (MethodInfo* mi = ic::g_ic.class_get_method_from_name(c, "MatchInputKeyType", -1))
        g_ctrl.match_key_type = ic::MethodPtr(mi);
    if (MethodInfo* mi = ic::g_ic.class_get_method_from_name(c, "ResetAllKeyState", 0))
        g_ctrl.reset_all = ic::MethodPtr(mi);

    LOGI("VehicleInputCtrl: SetKeyState=%p SetKeyStateFlags=%p MatchInputKeyType=%p ResetAll=%p",
         g_ctrl.set_key_state, g_ctrl.set_key_flags, g_ctrl.match_key_type, g_ctrl.reset_all);
    return g_ctrl.set_key_state || g_ctrl.set_key_flags;
}

static void apply_mask(uint32_t mask, uint32_t prev) {
    if (!g_ctrl.instance) return;
    uint32_t changed = mask ^ prev;
    if (!changed) return;
    // 优先用 flags 一次性写（最稳，不受单键 API 变更影响）
    if (g_ctrl.set_key_flags) {
        using Fn = void (*)(void*, uint32_t);
        reinterpret_cast<Fn>(g_ctrl.set_key_flags)(g_ctrl.instance, mask);
        return;
    }
    if (g_ctrl.set_key_state) {
        using Fn = void (*)(void*, int, bool);
        for (int bit = 0; bit < 5; bit++) {
            if (!(changed & (1u << bit))) continue;
            reinterpret_cast<Fn>(g_ctrl.set_key_state)(g_ctrl.instance, bit, (mask >> bit) & 1u);
        }
    }
}

// ---------------------------------------------------------------- 回放格式
// 极简二进制：每帧 8 字节 = [uint32 t_ms 相对首帧][uint32 mask]
#pragma pack(push, 1)
struct Frame {
    uint32_t t;
    uint32_t mask;
};
#pragma pack(pop)

static void* tick_thread(void*) {
    if (!ic::Load("libil2cpp.so")) {
        LOGE("dlopen libil2cpp failed: %s", dlerror());
        return nullptr;
    }
    LOGI("il2cpp bound (deferred)");
    // 等 il2cpp 初始化完成（游戏主线程跑起来之后 domain 才非空）
    Il2CppDomain* dom = nullptr;
    for (int i = 0; i < 900; i++) {
        if (ic::g_ic.domain_get) dom = ic::g_ic.domain_get();
        if (dom) break;
        usleep(100 * 1000);
    }
    if (!dom) {
        LOGE("il2cpp domain never appeared");
        return nullptr;
    }
    ic::g_ic.thread_attach(dom);
    LOGI("attached to il2cpp domain");

    for (int attempt = 0; !resolve_vehicle_ctrl(); attempt++) {
        if (attempt % 20 == 0) LOGE("VehicleInputCtrl not resolved yet (attempt %d)", attempt);
        usleep(300 * 1000);
    }
    g_ctrl.instance = find_scene_object(g_ctrl.cls);
    LOGI("instance = %p", g_ctrl.instance);

    // -------- 回放 --------
    if (g_mode == 1) {
        FILE* f = fopen(g_replay_path, "rb");
        if (!f) { LOGE("open replay failed: %s", g_replay_path); return nullptr; }
        std::vector<Frame> frames;
        Frame fr;
        while (fread(&fr, sizeof(fr), 1, f) == 1) frames.push_back(fr);
        fclose(f);
        LOGI("replay frames = %zu", frames.size());

        if (!g_ctrl.instance) g_ctrl.instance = find_scene_object(g_ctrl.cls);
        double t0 = now_ms();
        uint32_t prev = 0;
        size_t i = 0;
        while (i < frames.size()) {
            double el = now_ms() - t0;
            while (i < frames.size() && frames[i].t <= el) {
                apply_mask(frames[i].mask, prev);
                prev = frames[i].mask;
                i++;
            }
            usleep(8 * 1000);
            if (!g_ctrl.instance) g_ctrl.instance = find_scene_object(g_ctrl.cls);
        }
        apply_mask(0, prev);
        LOGI("replay done");
        return nullptr;
    }

    // -------- 录制 --------
    if (g_mode == 2) {
        FILE* f = fopen(g_replay_path, "wb");
        if (!f) { LOGE("create replay failed"); return nullptr; }
        double t0 = now_ms();
        uint32_t prev = 0;
        while (g_running) {
            uint32_t m = g_mask;
            if (m != prev) {
                Frame fr{static_cast<uint32_t>(now_ms() - t0), m};
                fwrite(&fr, sizeof(fr), 1, f);
                prev = m;
            }
            usleep(8 * 1000);
        }
        fclose(f);
        LOGI("record done");
        return nullptr;
    }
    return nullptr;
}

// ---------------------------------------------------------------- 外部控制
// 通过写 /data/local/tmp/qqauto.cmd 文本文件下发指令（不依赖 JNI/UI）：
//   mode=replay|record|idle
//   mask=0x3  (仅在需要在外部实时推 mask 时用)
static volatile int g_use_cli = 0;

static void* cli_thread(void*) {
    const char* cmd = "/data/local/tmp/qqauto.cmd";
    while (true) {
        FILE* f = fopen(cmd, "r");
        if (f) {
            char buf[256] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            (void)n;
            if (strstr(buf, "mode=replay")) g_mode = 1;
            else if (strstr(buf, "mode=record")) g_mode = 2;
            else if (strstr(buf, "mode=idle")) g_mode = 0;
            if (char* p = strstr(buf, "mask=")) g_mask = (uint32_t)strtoul(p + 5, nullptr, 0);
            unlink(cmd);
        }
        usleep(150 * 1000);
    }
    return nullptr;
}

static void* cli_thread(void*);

static void* tick_thread(void*);

// SIGALRM 触发后再启动工作线程：此时 linker 已完成初始化，创建线程/调 dlopen 都安全
static void on_alarm(int) {
    alarm(0);
    LOGI("alarm fired -> starting workers");
    pthread_t t1, t2;
    if (pthread_create(&t1, nullptr, cli_thread, nullptr) == 0) pthread_detach(t1);
    else LOGE("cli thread create failed");
    if (pthread_create(&t2, nullptr, tick_thread, nullptr) == 0) pthread_detach(t2);
    else LOGE("tick thread create failed");
}

extern "C" __attribute__((constructor)) void qqauto_init() {
    LOGI("qqauto loaded, pid=%d", getpid());
    g_running = 1;
    const char* env = getenv("QQAUTO_MODE");
    if (env) {
        if (strcmp(env, "replay") == 0) g_mode = 1;
        else if (strcmp(env, "record") == 0) g_mode = 2;
    } else {
        g_mode = 1;  // 默认回放
    }

    // ★ 关键（实测得出）：constructor 运行在 linker 初始化期。
    //   此时 pthread_create / dlopen("libil2cpp.so") 会让目标进程被系统杀掉
    //   （注入瞬间 verity: ALIVE=0）。必须只 arm 一个定时器就返回，
    //   由 SIGALRM 在 linker 空闲后再拉起工作线程。
    signal(SIGALRM, on_alarm);
    alarm(5);
    LOGI("ctor done, armed alarm(5) for deferred start");
}
