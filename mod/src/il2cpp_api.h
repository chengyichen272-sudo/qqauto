// il2cpp_api.h — libil2cpp 运行时符号绑定（arm64-v8a）
// 只声明本模块用到的部分，全部通过 dlsym 在运行时拿，不依赖任何离线 dump。
#pragma once
#include <stddef.h>
#include <stdint.h>

struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppObject;
struct Il2CppType;
struct Il2CppString;
struct MethodInfo;
struct FieldInfo;

namespace ic {

struct Api {
    Il2CppDomain* (*domain_get)();
    const Il2CppAssembly** (*domain_get_assemblies)(const Il2CppDomain*, size_t*);
    const Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*);
    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char* namespaze, const char* name);
    size_t (*image_get_class_count)(const Il2CppImage*);
    Il2CppClass* (*image_get_class)(const Il2CppImage*, size_t);
    FieldInfo* (*class_get_fields)(Il2CppClass*, void** iter);
    const char* (*field_get_name)(FieldInfo*);
    int (*field_get_flags)(FieldInfo*);
    MethodInfo* (*class_get_methods)(Il2CppClass*, void** iter);
    MethodInfo* (*class_get_method_from_name)(Il2CppClass*, const char*, int argc);
    FieldInfo* (*class_get_field_from_name)(Il2CppClass*, const char*);
    size_t (*field_get_offset)(FieldInfo*);
    const char* (*method_get_name)(const MethodInfo*);
    const char* (*class_get_name)(Il2CppClass*);
    const char* (*class_get_namespace)(Il2CppClass*);
    int (*method_get_param_count)(const MethodInfo*);
    Il2CppString* (*string_new)(const char*);
    Il2CppClass* (*thread_attach)(Il2CppDomain*);
    void (*thread_detach)(Il2CppDomain*);
    Il2CppType* (*class_get_type)(Il2CppClass*);
    Il2CppObject* (*type_get_object)(const Il2CppType*);
    Il2CppObject* (*runtime_invoke)(const MethodInfo*, void* obj, void** params, Il2CppObject** exc);
    void (*field_static_get_value)(FieldInfo*, void* out);
    uint32_t (*gchandle_new)(Il2CppObject*, bool pinned);
    Il2CppObject* (*gchandle_get_target)(uint32_t);
    Il2CppClass* (*object_get_class)(Il2CppObject*);
};

extern Api g_ic;

// 在进程内解析 libil2cpp 的导出符号（libil2cpp 此时已经被 linker 加载）
bool Load(const char* so_name = "libil2cpp.so");

inline void* MethodPtr(const MethodInfo* mi) { return *reinterpret_cast<void* const*>(mi); }

}  // namespace ic
