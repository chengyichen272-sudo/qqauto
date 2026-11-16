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
#include <stdint.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <setjmp.h>
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

// ---------------------------------------------------------------- libil2cpp 定位
// 这个游戏的 libil2cpp.so 不解压到文件系统，而是从 split_config.arm64_v8a.apk 里
// 直接 mmap，所以：
//   dlopen("libil2cpp.so")      -> library not found
//   dlopen(apk!/lib/.../lib.so) -> RTLD_NOW 会立即解析全部符号 -> undefined symbol
// 可靠做法：dl_iterate_phdr 拿到 load bias，再按 .dynsym 的 st_value 直接算地址。
static char g_il2cpp_path[512] = {0};
static uintptr_t g_il2cpp_bias = 0;

static int find_il2cpp_cb(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && info->dlpi_name[0] &&
        strstr(info->dlpi_name, "libil2cpp")) {
        g_il2cpp_bias = static_cast<uintptr_t>(info->dlpi_addr);
        snprintf(g_il2cpp_path, sizeof(g_il2cpp_path), "%s", info->dlpi_name);
        return 1;
    }
    return 0;
}

struct SymOff { const char* name; uintptr_t off; };

// 偏移取自当前版本 libil2cpp.so 的 .dynsym（tools/elf_sym.py 生成）。
// 游戏更新后重新 dump 一次即可，其它逻辑不用动。
static const SymOff kSyms[] = {
    {"il2cpp_domain_get", 0x43bb844},
    {"il2cpp_domain_get_assemblies", 0x43bb850},
    {"il2cpp_assembly_get_image", 0x43bb210},
    {"il2cpp_class_from_name", 0x43bb23c},
    {"il2cpp_image_get_class_count", 0x43bbe08},
    {"il2cpp_image_get_class", 0x43bbe0c},
    {"il2cpp_class_get_fields", 0x43bb248},
    {"il2cpp_field_get_name", 0x43bb978},
    {"il2cpp_field_get_flags", 0x43bb97c},
    {"il2cpp_class_get_methods", 0x43bb260},
    {"il2cpp_class_get_method_from_name", 0x43bb264},
    {"il2cpp_class_get_field_from_name", 0x43bb25c},
    {"il2cpp_field_get_offset", 0x43bb984},
    {"il2cpp_method_get_name", 0x43bbac0},
    {"il2cpp_class_get_name", 0x43bb268},
    {"il2cpp_class_get_namespace", 0x43bb26c},
    {"il2cpp_method_get_param_count", 0x43bbad0},
    {"il2cpp_string_new", 0x43bbbf4},
    {"il2cpp_thread_attach", 0x43bbc10},
    {"il2cpp_thread_detach", 0x43bbc14},
    {"il2cpp_class_get_type", 0x43bb2a4},
    {"il2cpp_type_get_object", 0x43bbc6c},
    {"il2cpp_runtime_invoke", 0x43bbbc0},
    {"il2cpp_field_static_get_value", 0x43bb9a0},
    {"il2cpp_gchandle_new", 0x43bba20},
    {"il2cpp_object_get_class", 0x43bbb50},
    {"il2cpp_gchandle_get_target", 0x43bba30},
};

static void* sym_by_offset(const char* name) {
    if (!g_il2cpp_bias) return nullptr;
    for (const auto& s : kSyms)
        if (strcmp(s.name, name) == 0)
            return reinterpret_cast<void*>(g_il2cpp_bias + s.off);
    return nullptr;
}

template <typename T>
static bool bind(void* h, const char* name, T& fn) {
    void* p = h ? dlsym(h, name) : nullptr;
    if (!p) p = sym_by_offset(name);
    fn = reinterpret_cast<T>(p);
    if (!fn) LOGE("resolve miss: %s", name);
    return fn != nullptr;
}

bool Load(const char* so_name) {
    // 1) 定位模块，拿 load bias（后面无论走哪条路都要用）
    dl_iterate_phdr(find_il2cpp_cb, nullptr);
    if (!g_il2cpp_bias) {
        LOGE("libil2cpp not mapped yet");
        return false;
    }
    LOGI("libil2cpp bias=0x%lx", static_cast<unsigned long>(g_il2cpp_bias));
    LOGI("libil2cpp path=%s", g_il2cpp_path);

    // 2) 能拿到句柄就优先用 dlsym（按名字更抗更新）
    void* h = dlopen(so_name, RTLD_NOW | RTLD_NOLOAD);
    if (!h && g_il2cpp_path[0]) {
        h = dlopen(g_il2cpp_path, RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen(g_il2cpp_path, RTLD_LAZY);
        if (!h) LOGI("dlopen(path) failed (%s), fallback to bias+offset", dlerror());
    }
    if (!h) LOGI("no handle, using bias+offset table");

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
    ok &= bind(h, "il2cpp_class_get_fields", g_ic.class_get_fields);
    ok &= bind(h, "il2cpp_field_get_name", g_ic.field_get_name);
    ok &= bind(h, "il2cpp_field_get_flags", g_ic.field_get_flags);
    ok &= bind(h, "il2cpp_class_get_methods", g_ic.class_get_methods);
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
    ok &= bind(h, "il2cpp_object_get_class", g_ic.object_get_class);
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
static bool g_dumped_find = false;
static MethodInfo* g_mi_find_all = nullptr;   // FindObjectsOfTypeAll(Type)

// 枚举 UnityEngine.Object 上所有 Find* 方法，先把真实签名打出来
static void dump_find_methods(Il2CppClass* uo) {
    if (g_dumped_find || !uo || !ic::g_ic.class_get_methods) return;
    g_dumped_find = true;
    void* iter = nullptr;
    const MethodInfo* mi;
    int n = 0;
    while ((mi = ic::g_ic.class_get_methods(uo, &iter)) != nullptr && n < 400) {
        const char* name = ic::g_ic.method_get_name(mi);
        if (name && strstr(name, "Find")) {
            const int ac = ic::g_ic.method_get_param_count(mi);
            LOGI("  Object.%s  argc=%d", name, ac);
            if (strcmp(name, "FindObjectsOfTypeAll") == 0 && ac == 1 && !g_mi_find_all) {
                g_mi_find_all = const_cast<MethodInfo*>(mi);
                LOGI("  -> captured FindObjectsOfTypeAll MethodInfo");
            }
        }
        n++;
    }
    LOGI("dump_find_methods done, scanned %d methods", n);
}

static Il2CppObject* invoke_find(Il2CppClass* uo, const char* name, int argc,
                                 Il2CppObject* sys_type, void** args, bool* ok) {
    *ok = false;
    MethodInfo* mi = ic::g_ic.class_get_method_from_name(uo, name, argc);
    if (!mi) return nullptr;
    Il2CppObject* exc = nullptr;
    Il2CppObject* obj = ic::g_ic.runtime_invoke(mi, nullptr, args, &exc);
    if (exc) {
        LOGE("%s(argc=%d) threw: %p", name, argc, exc);
        return nullptr;
    }
    *ok = (obj != nullptr);
    return obj;
}

static Il2CppObject* find_scene_object(Il2CppClass* cls) {
    if (!g_uo_cls) g_uo_cls = find_class_by_name("Object", "UnityEngine");
    Il2CppClass* uo = g_uo_cls;
    if (!uo) { LOGE("UnityEngine.Object not found"); return nullptr; }

    dump_find_methods(uo);

    Il2CppType* t = ic::g_ic.class_get_type(cls);
    if (!t) { LOGE("class_get_type failed"); return nullptr; }
    Il2CppObject* sys_type = ic::g_ic.type_get_object(t);
    if (!sys_type) { LOGE("type_get_object failed"); return nullptr; }

    static bool s_false = false;
    void* args[2] = {sys_type, &s_false};
    bool ok = false;

    // A) 先看看目标类有没有静态字段（只打印，不做类型未知的静态读取，避免越界）
    if (ic::g_ic.class_get_fields) {
        void* it = nullptr;
        FieldInfo* fi;
        int fn = 0;
        while ((fi = ic::g_ic.class_get_fields(cls, &it)) != nullptr && fn < 2000) {
            const char* fname = ic::g_ic.field_get_name(fi);
            const int flags = ic::g_ic.field_get_flags(fi);
            if (flags & 0x0010) {   // FIELD_ATTRIBUTE_STATIC
                LOGI("  static field: %s (flags=0x%x)", fname ? fname : "?", flags);
            }
            fn++;
        }
        LOGI("fields scanned: %d", fn);
    }

    // B) 内存扫描：对象首字段 = klass，次字段(monitor) 通常为 0
    {
        install_segv_guard();
        FILE* mf = fopen("/proc/self/maps", "r");
        if (mf) {
            char line[512];
            const uintptr_t pat = reinterpret_cast<uintptr_t>(cls);
            size_t scanned = 0;
            const size_t LIMIT = (size_t)512 << 20;   // 最多 512MB
            int regions = 0;
            while (fgets(line, sizeof(line), mf) && scanned < LIMIT) {
                uintptr_t a = 0, b = 0;
                char perms[8] = {0};
                char path[256] = {0};
                sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &a, &b, perms, path);
                if (perms[0] != 'r' || perms[1] != 'w') continue;
                if (strstr(path, "dalvik") || strstr(path, "[stack]") ||
                    strstr(path, "[vectors]") || strstr(path, "[vdso]")) continue;
                if (b <= a) continue;
                regions++;

                if (sigsetjmp(g_segv_jmp, 1) == 0) {
                    g_segv_armed = 1;
                    for (uintptr_t q = a; q + 16 <= b; q += 8) {
                        uintptr_t v = *reinterpret_cast<uintptr_t*>(q);
                        if (v == pat && *reinterpret_cast<uintptr_t*>(q + 8) == 0) {
                            g_segv_armed = 0;
                            LOGI("scan hit: obj=0x%lx", static_cast<unsigned long>(q));
                            fclose(mf);
                            return reinterpret_cast<Il2CppObject*>(q);
                        }
                    }
                    g_segv_armed = 0;
                } else {
                    LOGI("scan: skipped faulting region 0x%lx-0x%lx",
                         static_cast<unsigned long>(a), static_cast<unsigned long>(b));
                }
                scanned += (b - a);
            }
            fclose(mf);
            LOGI("scan done: %d regions, %zu MB, no hit", regions, scanned >> 20);
        } else {
            LOGE("cannot open /proc/self/maps");
        }
    }

    LOGE("no scene object found for class");
    return nullptr;
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

// ---------------------------------------------------------------- 扫描保护
// 直接读进程内存可能撞到已 munmap / 受保护的区域（游戏在跑，GC 会释放）。
// 用 SIGSEGV + siglongjmp 兜底，跳过坏页而不是崩进程。
static sigjmp_buf g_segv_jmp;
static volatile sig_atomic_t g_segv_armed = 0;
static void (*g_old_segv)(int) = nullptr;

static void segv_handler(int sig) {
    if (g_segv_armed) {
        g_segv_armed = 0;
        siglongjmp(g_segv_jmp, 1);
    }
    if (g_old_segv && g_old_segv != segv_handler) g_old_segv(sig);
}

static void install_segv_guard() {
    g_old_segv = signal(SIGSEGV, segv_handler);
    signal(SIGBUS, segv_handler);
}

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
    // 实例只在进入比赛后才有，持续重试直到拿到（也可以等一下再找）
    int tries = 0;
    while (!g_ctrl.instance && tries < 600) {
        g_ctrl.instance = find_scene_object(g_ctrl.cls);
        if (g_ctrl.instance) {
            LOGI("instance acquired: %p", g_ctrl.instance);
            break;
        }
        if (tries % 10 == 0) LOGI("waiting for scene object... (%d)", tries);
        tries++;
        usleep(1000 * 1000);
    }
    if (!g_ctrl.instance) {
        LOGE("give up waiting for scene object");
    }

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
