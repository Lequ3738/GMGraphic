// 双后端检测: 判断 GM8 runner 的 D3D 设备是 D3D8 还是 D3D9。GMDirectX9 插件下 0x58d388 是 IDirect3DDevice9,
// 原生 GM8 是 IDirect3DDevice8(vtable 槽位不同)。权威判据: 查设备 vtable 指针落在 d3d9.dll(→V9)还是 d3d8.dll(→V8)。
#include "d3d_adapter.h"

namespace d3d
{
    static int   g_ver = UNKNOWN;
    static void* g_dev = nullptr;   // 原始 COM 设备指针(IDirect3DDevice8/9 对象)
    static void* g_ifc = nullptr;   // 原始 COM 接口指针(IDirect3D8/9 对象)

    void* device() { return g_dev; }
    void* iface()  { return g_ifc; }

    int version()
    {
        if (g_ver == UNKNOWN && g_dev)
            ensure_version(g_dev, g_ifc);
        return g_ver == UNKNOWN ? V8 : g_ver;   // 未初始化默认 D3D8(历史行为)
    }

    void ensure_version(void* device_, void* iface_)
    {
        g_dev = device_;
        g_ifc = iface_;

        HMODULE mod = nullptr;
        if (g_dev && GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)(*(void**)g_dev), &mod))
        {
            wchar_t path[MAX_PATH];
            if (mod && GetModuleFileNameW(mod, path, MAX_PATH))
            {
                if (wcsstr(path, L"d3d9.dll"))      g_ver = V9;
                else if (wcsstr(path, L"d3d8.dll")) g_ver = V8;
            }
        }

        // 兜底: 检测失败时看 d3d9.dll 是否已在进程里(GMDirectX9 打补丁即加载)。
        if (g_ver == UNKNOWN)
            g_ver = GetModuleHandleW(L"d3d9.dll") ? V9 : V8;
    }
}
