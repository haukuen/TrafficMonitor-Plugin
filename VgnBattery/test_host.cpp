// test_host.cpp — 模拟 TrafficMonitor 宿主, 验证插件 DLL 的接口与 HID 查询
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>

#include "PluginInterface.h"

typedef ITMPlugin* (*PF_GetInstance)();

int wmain() {
    _setmode(_fileno(stdout), _O_U16TEXT);

    HMODULE dll = LoadLibraryW(L"VgnBattery.dll");
    if (dll == nullptr) {
        wprintf(L"LoadLibrary 失败: %lu\n", GetLastError());
        return 1;
    }
    auto get_instance = (PF_GetInstance)GetProcAddress(dll, "TMPluginGetInstance");
    if (get_instance == nullptr) {
        wprintf(L"没有导出 TMPluginGetInstance\n");
        return 1;
    }

    ITMPlugin* plugin = get_instance();
    wprintf(L"插件已加载: %ls  API=%d  版本 %ls\n",
            plugin->GetInfo(ITMPlugin::TMI_NAME), plugin->GetAPIVersion(),
            plugin->GetInfo(ITMPlugin::TMI_VERSION));
    plugin->OnInitialize(nullptr);   // 启动后台轮询线程

    IPluginItem* item = plugin->GetItem(0);
    if (item == nullptr) { wprintf(L"GetItem(0) 返回空\n"); return 1; }
    wprintf(L"显示项: %ls (%ls)  标签=%ls  样例=%ls\n\n",
            item->GetItemName(), item->GetItemId(),
            item->GetItemLableText(), item->GetItemValueSampleText());

    for (int i = 1; i <= 10; ++i) {
        plugin->DataRequired();   // TrafficMonitor 周期调用
        wprintf(L"[t=%02ds] 数值=%-8ls 占用图=%.2f\n提示: %ls\n\n",
                i * 3, item->GetItemValueText(),
                item->GetResourceUsageGraphValue(), plugin->GetTooltipInfo());
        Sleep(3000);
    }

    FreeLibrary(dll);   // 触发 DllMain 卸载清理, 验证线程能干净退出
    wprintf(L"DLL 已卸载, 测试结束\n");
    return 0;
}
