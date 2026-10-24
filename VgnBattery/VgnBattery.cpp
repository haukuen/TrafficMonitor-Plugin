// VgnBattery.cpp
// TrafficMonitor 插件: 在任务栏显示 VGN (Telink 方案) 无线键盘的电量
//
//   通道  : VID 0x320F, 接收器复合接口上 usage page 0xFF1C / usage 0x0092
//           的 64 字节带编号 report 通道 (N75 PRO 2.4G Dongle 的 MI_01)
//   传输  : output report 0x04 发命令, input report 0x04 收响应
//   电量  : 发送载荷 00 00 1A 06 00 00 00, 响应载荷 [7] 为电量百分比,
//           [8] 非 0 为充电中 (电量上涨时实测为 1, 平时为 0)
//   休眠  : 响应载荷 [2] = 0xFF 表示键盘休眠暂无数据 (约 1s 后才回),
//           唤醒后重试即得; 与官方驱动行为一致
//
// 针对 TrafficMonitor V1.86 (插件 API version 7, x64), 纯 Win32 无 MFC 依赖。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <initguid.h>
#include <hidclass.h>

#include <atomic>
#include <cwchar>
#include <string>
#include <vector>

#include "PluginInterface.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// ── VGN (Telink) 协议常量 ───────────────────────────────────────────────────
static constexpr USHORT VGN_VID = 0x320F;
static constexpr USAGE  CFG_USAGE_PAGE = 0xFF1C;  // 厂商自定义页
static constexpr USAGE  CFG_USAGE = 0x0092;
static constexpr BYTE   REPORT_ID = 0x04;
static constexpr DWORD  REPORT_LEN = 64;          // report id + 63 字节载荷
static constexpr DWORD  REPLY_WAIT_MS = 1500;     // 键盘休眠时响应延迟约 1s
static constexpr int    BUSY_ATTEMPTS = 4;        // 单轮查询对休眠的重试次数

static constexpr DWORD POLL_OK_MS = 60000;        // 拿到数据后的轮询间隔
static constexpr DWORD POLL_RETRY_MS = 5000;      // 无设备/休眠时的轮询间隔

// 匹配 VGN 配置通道: FF1C/0092 的 64 字节带编号 in/out 报文集合。
// 不锁定 PID —— 有线模式的 PID 与接收器 (0x5088) 不同, 按通道形状识别。
static bool IsVgnConfigChannel(const HIDP_CAPS& caps) {
    return caps.UsagePage == CFG_USAGE_PAGE && caps.Usage == CFG_USAGE
        && caps.InputReportByteLength == REPORT_LEN
        && caps.OutputReportByteLength == REPORT_LEN;
}

// ── 全局状态 ────────────────────────────────────────────────────────────────
enum class LinkState { NO_DEVICE, SLEEP, LINKED };

struct BatteryState {
    bool valid = false;      // 读到过真实电量
    int battery = 0;
    bool charging = false;
    wchar_t product[128] = L"VGN";
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static BatteryState g_state;
static wchar_t g_text_buf[32] = L"--";
static wchar_t g_sample_buf[32] = L"88 %⚡";
static wchar_t g_tooltip[256] = L"VGN 键盘电量\n正在查找设备...";
static float g_graph = 0.0f;
static std::atomic<LinkState> g_link_state{ LinkState::NO_DEVICE };

// TM 通过 OnExtenedInfo 推送的显示设置
static std::atomic<bool> g_space_taskbar{ true };
static std::atomic<bool> g_space_main{ true };
static std::atomic<bool> g_draw_taskbar{ true };

static HANDLE g_thread = nullptr;
static HANDLE g_stop_event = nullptr;
static std::atomic<bool> g_running{ false };

// ── 帧编解码 ────────────────────────────────────────────────────────────────
// 电量查询帧: report id 0x04 + 载荷 00 00 1A 06 00 00 00 (其余补零)
static void BuildQueryFrame(BYTE* buf) {
    memset(buf, 0, REPORT_LEN);
    buf[0] = REPORT_ID;
    buf[3] = 0x1A;
    buf[4] = 0x06;
}

enum class ParseResult { OK, BUSY, MISMATCH };

static ParseResult ParseReply(const BYTE* report, DWORD len, BatteryState& out) {
    if (len < 10 || report[0] != REPORT_ID) return ParseResult::MISMATCH;
    const BYTE* p = report + 1;                          // 载荷, 不含 report id
    if (p[2] == 0xFF) return ParseResult::BUSY;          // 键盘休眠, 无数据
    if (p[2] != 0x1A) return ParseResult::MISMATCH;      // 其他命令/广播, 忽略
    if (p[7] > 100) return ParseResult::MISMATCH;        // 电量不可能 >100
    out.battery = p[7];
    out.charging = p[8] != 0;
    return ParseResult::OK;
}

// ── HID 设备发现 ────────────────────────────────────────────────────────────
struct DeviceEntry {
    HANDLE h = nullptr;
    wchar_t product[128] = L"VGN";
};

static void OpenVgnConfigDevices(std::vector<DeviceEntry>& out) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO dev_info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return;

    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA if_data = {};
        if_data.cbSize = sizeof(if_data);
        if (!SetupDiEnumDeviceInterfaces(dev_info, nullptr, &hid_guid, i, &if_data))
            break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(need);
        if (detail == nullptr) break;
        detail->cbSize = sizeof(*detail);
        BOOL ok = SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, detail,
                                                   need, nullptr, nullptr);

        HANDLE h = nullptr;
        if (ok) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) h = nullptr;
        }
        if (h != nullptr) {
            HIDD_ATTRIBUTES attr = {};
            attr.Size = sizeof(attr);
            PHIDP_PREPARSED_DATA prep = nullptr;
            if (HidD_GetAttributes(h, &attr) && attr.VendorID == VGN_VID
                && HidD_GetPreparsedData(h, &prep) && prep != nullptr) {
                HIDP_CAPS caps = {};
                if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS
                    && IsVgnConfigChannel(caps)) {
                    DeviceEntry e;
                    e.h = h;
                    e.product[0] = L'\0';
                    HidD_GetProductString(h, e.product, sizeof(e.product));
                    if (e.product[0] == L'\0') wcscpy_s(e.product, L"VGN Keyboard");
                    out.push_back(e);
                    h = nullptr;   // 句柄已移交给列表
                }
            }
            if (prep != nullptr) HidD_FreePreparsedData(prep);
            if (h != nullptr) CloseHandle(h);
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(dev_info);
}

// ── 一次查询: 发命令并等响应 ────────────────────────────────────────────────
static ParseResult QueryOnce(HANDLE h, HANDLE io_event, BatteryState& out) {
    BYTE frame[REPORT_LEN];
    BuildQueryFrame(frame);

    OVERLAPPED ov = {};
    ov.hEvent = io_event;
    DWORD written = 0;
    if (!WriteFile(h, frame, sizeof(frame), &written, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return ParseResult::MISMATCH;
        if (WaitForSingleObject(io_event, 500) != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            return ParseResult::MISMATCH;
        }
    }

    ULONGLONG end_tick = GetTickCount64() + REPLY_WAIT_MS;
    while (GetTickCount64() < end_tick) {
        BYTE report[REPORT_LEN];
        DWORD read_n = 0;
        OVERLAPPED rov = {};
        rov.hEvent = io_event;
        if (!ReadFile(h, report, sizeof(report), &read_n, &rov)) {
            if (GetLastError() != ERROR_IO_PENDING) return ParseResult::MISMATCH;
            // 分片等待, 便于及时放弃
            ULONGLONG slice_end = GetTickCount64() + 300;
            bool got = false;
            while (GetTickCount64() < slice_end
                   && GetTickCount64() < end_tick) {
                if (WaitForSingleObject(io_event, 100) == WAIT_OBJECT_0) { got = true; break; }
            }
            if (!got) {
                CancelIoEx(h, &rov);
                WaitForSingleObject(io_event, 200);
                break;
            }
            if (!GetOverlappedResult(h, &rov, &read_n, FALSE)) return ParseResult::MISMATCH;
        }
        ParseResult r = ParseReply(report, read_n, out);
        if (r != ParseResult::MISMATCH) return r;
        // 命令号不匹配的报文 (固件状态广播) 忽略, 继续读
    }
    return ParseResult::MISMATCH;
}

// ── 缓存更新 (工作线程调用) ─────────────────────────────────────────────────
static void PublishState(const BatteryState& st, LinkState link) {
    AcquireSRWLockExclusive(&g_lock);
    g_state = st;
    g_graph = st.valid ? st.battery / 100.0f : 0.0f;
    switch (link) {
    case LinkState::LINKED:
        swprintf_s(g_tooltip, L"%ls\r\n电量 %d %% · %ls", st.product, st.battery,
                   st.charging ? L"充电中" : L"未充电");
        break;
    case LinkState::SLEEP:
        if (st.valid)
            swprintf_s(g_tooltip, L"%ls\r\n电量 %d %% · 键盘休眠中, 显示上次数据",
                       st.product, st.battery);
        else
            swprintf_s(g_tooltip, L"%ls\r\n键盘休眠/离线, 敲一下键盘即可", st.product);
        break;
    default:
        wcscpy_s(g_tooltip, L"未找到 VGN 键盘 (VID 320F)");
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// ── 工作线程: 轮询电量 ──────────────────────────────────────────────────────
static DWORD WINAPI WorkerProc(LPVOID) {
    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        std::vector<DeviceEntry> devices;
        OpenVgnConfigDevices(devices);
        if (devices.empty()) {
            g_link_state = LinkState::NO_DEVICE;
            BatteryState st;
            PublishState(st, LinkState::NO_DEVICE);
            if (WaitForSingleObject(g_stop_event, POLL_RETRY_MS) != WAIT_TIMEOUT)
                break;
            continue;
        }
        // 自动复位事件: 每次 IO 完成后信号被消费, 不会串扰下一次等待
        HANDLE io_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        int sticky = -1;          // 上一轮应答的设备索引 (多连接方式间保持)
        BatteryState last_good;   // 休眠时保留上次的真实数据
        bool device_ok = true;    // 全部 IO 失败 -> 重新枚举

        while (device_ok && WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
            // 查询顺序: 上次应答的设备优先, 其余按发现序
            int order[8];
            int order_n = 0;
            for (int n = 0; n < (int)devices.size() && order_n < 8; ++n)
                order[order_n++] = n;
            if (sticky >= 0) {
                for (int n = 0; n < order_n; ++n)
                    if (order[n] == sticky) {
                        for (int m = n; m > 0; --m) order[m] = order[m - 1];
                        order[0] = sticky;
                        break;
                    }
            }

            bool any_busy = false, any_iofail = false;
            bool got_ok = false;
            BatteryState fresh;
            for (int n = 0; n < order_n && !got_ok; ++n) {
                DeviceEntry& dev = devices[order[n]];
                BatteryState cur;
                wcscpy_s(cur.product, dev.product);
                ParseResult r = ParseResult::MISMATCH;
                for (int attempt = 0; attempt < BUSY_ATTEMPTS; ++attempt) {
                    r = QueryOnce(dev.h, io_event, cur);
                    if (r != ParseResult::BUSY) break;
                    if (WaitForSingleObject(g_stop_event, 300) != WAIT_TIMEOUT) break;
                }
                switch (r) {
                case ParseResult::OK:
                    cur.valid = true;
                    fresh = cur;
                    last_good = cur;
                    sticky = order[n];
                    got_ok = true;
                    break;
                case ParseResult::BUSY:  any_busy = true; break;
                default:                  any_iofail = true; break;
                }
            }

            if (got_ok) {
                g_link_state = LinkState::LINKED;
                PublishState(fresh, LinkState::LINKED);
                if (WaitForSingleObject(g_stop_event, POLL_OK_MS) != WAIT_TIMEOUT)
                    break;
            } else if (any_busy) {
                // 键盘休眠: 有上次数据就继续显示, 否则提示敲一下键盘
                g_link_state = LinkState::SLEEP;
                if (last_good.valid)
                    PublishState(last_good, LinkState::SLEEP);
                else {
                    BatteryState hint;
                    wcscpy_s(hint.product, devices[0].product);
                    PublishState(hint, LinkState::SLEEP);
                }
                if (WaitForSingleObject(g_stop_event, POLL_RETRY_MS) != WAIT_TIMEOUT)
                    break;
            } else {
                // 所有设备都 IO 失败: 可能被拔出, 回到重新发现
                device_ok = false;
            }
        }
        if (io_event) CloseHandle(io_event);
        for (auto& dev : devices)
            if (dev.h) CloseHandle(dev.h);
        if (WaitForSingleObject(g_stop_event, POLL_RETRY_MS) != WAIT_TIMEOUT)
            break;
    }
    return 0;
}

// ── IPluginItem 实现 ────────────────────────────────────────────────────────
class CBatteryItem : public IPluginItem {
public:
    const wchar_t* GetItemName() const override { return L"键盘电量"; }
    const wchar_t* GetItemId() const override { return L"vgn_keyboard_battery"; }
    const wchar_t* GetItemLableText() const override { return L"键盘: "; }
    const wchar_t* GetItemValueText() const override {
        const bool space = g_draw_taskbar ? g_space_taskbar.load() : g_space_main.load();
        AcquireSRWLockShared(&g_lock);
        if (g_state.valid)
            swprintf_s(g_text_buf, L"%d%s%%%ls", g_state.battery,
                       space ? L" " : L"", g_state.charging ? L"⚡" : L"");
        else
            wcscpy_s(g_text_buf, L"--");
        const wchar_t* v = g_text_buf;
        ReleaseSRWLockShared(&g_lock);
        return v;
    }
    const wchar_t* GetItemValueSampleText() const override {
        AcquireSRWLockShared(&g_lock);
        swprintf_s(g_sample_buf, L"88%s%%⚡", g_space_taskbar.load() ? L" " : L"");
        const wchar_t* v = g_sample_buf;
        ReleaseSRWLockShared(&g_lock);
        return v;
    }
    int IsDrawResourceUsageGraph() const override { return 1; }
    float GetResourceUsageGraphValue() const override {
        AcquireSRWLockShared(&g_lock);
        float v = g_graph;
        ReleaseSRWLockShared(&g_lock);
        return v;
    }
};

// ── ITMPlugin 实现 ──────────────────────────────────────────────────────────
class CVgnPlugin : public ITMPlugin {
public:
    IPluginItem* GetItem(int index) override { return index == 0 ? &m_item : nullptr; }
    void DataRequired() override {}   // 数据由后台线程维护, 这里无需做事

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME:        return L"VGN 键盘电量";
        case TMI_DESCRIPTION: return L"显示 VGN (Telink 方案) 无线键盘的电量, "
                                    L"支持 2.4G 接收器连接, 直接 HID 通信无需官方驱动。";
        case TMI_AUTHOR:      return L"reverse-engineered with ZCode";
        case TMI_COPYRIGHT:   return L"MIT";
        case TMI_VERSION:     return L"1.0";
        case TMI_URL:         return L"https://hub.vgnlab.com.cn/";
        default:              return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override {
        AcquireSRWLockShared(&g_lock);
        const wchar_t* v = g_tooltip;
        ReleaseSRWLockShared(&g_lock);
        return v;
    }

    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override {
        switch (index) {
        case EI_TASKBAR_WND_SPERATE_WITH_SPACE:
            g_space_taskbar = wcscmp(data, L"1") == 0;
            break;
        case EI_MAIN_WND_SPERATE_WITH_SPACE:
            g_space_main = wcscmp(data, L"1") == 0;
            break;
        case EI_DRAW_TASKBAR_WND:
            g_draw_taskbar = wcscmp(data, L"1") == 0;
            break;
        default:
            break;
        }
    }

    void OnInitialize(ITrafficMonitor* /*pApp*/) override { StartWorker(); }

private:
    void StartWorker() {
        if (g_running.exchange(true)) return;
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ResetEvent(g_stop_event);
        g_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
        if (g_thread == nullptr) g_running = false;
    }

    CBatteryItem m_item;
};

static CVgnPlugin g_plugin;

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &g_plugin;
}

static void StopWorker() {
    if (!g_running.exchange(false)) return;
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_thread) {
        if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0) {
            // 读取单次最长 ~1.5s 且分片可中断, 5s 足够; 极端情况强制结束
            TerminateThread(g_thread, 0);
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop_event) { CloseHandle(g_stop_event); g_stop_event = nullptr; }
}

BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h_module);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // reserved 非空 = 进程正在终止, 无需 (也不能) 安全清理
        StopWorker();
    }
    return TRUE;
}
