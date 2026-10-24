// MchoseBattery.cpp
//
//   通道  : VID 0x3837, usage page 0xFF01 / usage 0x0001 的 HID collection
//   传输  : output report 0x4D 发命令, input report 0x4D 收响应, 帧尾 XOR 校验
//   电量  : 命令 0x0900, 响应载荷 [10]=连接 [11]=充电 [12]=电量% [13]=性能模式
//   休眠  : 载荷为单个 0xFF 表示接收器在线但鼠标无应答, 重试等待唤醒
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

// ── 迈从 V3 协议常量 ────────────────────────────────────────────────────────
static constexpr USHORT MCHOSE_VID = 0x3837;
static constexpr USAGE  CFG_USAGE_PAGE = 0xFF01;
static constexpr USAGE  CFG_USAGE = 0x0001;
static constexpr BYTE   REPORT_ID = 0x4D;
static constexpr BYTE   CMD_LO = 0x00;   // 命令 0x0900, 小端
static constexpr BYTE   CMD_HI = 0x09;
static constexpr DWORD  REPLY_WAIT_MS = 1200;  // 固件非即时响应固定约 1.001s
static constexpr int    BUSY_ATTEMPTS = 4;     // 单轮查询对 busy 的重试次数

static constexpr DWORD POLL_OK_MS = 60000;      // 拿到数据后的轮询间隔 (1 分钟)
static constexpr DWORD POLL_RETRY_MS = 5000;    // 无设备/休眠时的轮询间隔

// 接收器共用的 host PID (其余视为 USB 有线直连)
static bool IsReceiverPid(USHORT pid) {
    return pid == 0x1014 || pid == 0x1018 || pid == 0x1016;
}

// ── 全局状态 ────────────────────────────────────────────────────────────────
enum class LinkState { NO_DEVICE, BUSY, LINKED, REJECTED };

struct BatteryState {
    bool valid = false;      // 读到过真实电量
    int battery = 0;
    bool charging = false;
    bool linked = false;
    wchar_t product[128] = L"MCHOSE";
    wchar_t link[8] = L"";
};

static SRWLOCK g_lock = SRWLOCK_INIT;   // 保护 g_state / 文本缓冲
static BatteryState g_state;
static wchar_t g_text_buf[32] = L"--";       // GetItemValueText 返回缓冲
static wchar_t g_sample_buf[32] = L"88 %⚡"; // GetItemValueSampleText 返回缓冲
static wchar_t g_tooltip[256] = L"MCHOSE 鼠标电量\n正在查找设备...";
static float g_graph = 0.0f;
static std::atomic<LinkState> g_link_state{ LinkState::NO_DEVICE };

// TM 通过 OnExtenedInfo 推送的显示设置 (TrafficMonitor.cpp: SendSettingsToPlugin)
static std::atomic<bool> g_space_taskbar{ true };  // 任务栏: 数值和单位用空格分隔
static std::atomic<bool> g_space_main{ true };     // 主窗口: 同上
static std::atomic<bool> g_draw_taskbar{ true };   // 当前绘制上下文 (EI_DRAW_TASKBAR_WND)

static HANDLE g_thread = nullptr;
static HANDLE g_stop_event = nullptr;
static std::atomic<bool> g_running{ false };

// ── 帧编解码 ────────────────────────────────────────────────────────────────
static BYTE XorChecksum(const BYTE* body, int from, int to) {
    BYTE v = 0;
    for (int i = from; i <= to; ++i) v ^= body[i];
    return v;
}

// 组装完整 64 字节报文: [0]=0x4D + 63 字节 body
static void BuildFrame(BYTE* buf, BYTE seq) {
    memset(buf, 0, 64);
    buf[0] = REPORT_ID;
    BYTE* b = buf + 1;
    b[0] = 0x01;        // 协议版本
    b[1] = 0x01;        // 标志: 带校验
    b[2] = 0x00;        // 数据长度
    b[3] = CMD_LO;
    b[4] = CMD_HI;
    b[5] = 0x00;        // 业务码
    b[6] = seq;
    b[7] = XorChecksum(b, 1, 6);
}

enum class ParseResult { OK, BUSY, REJECTED, MISMATCH };

static ParseResult ParseReply(const BYTE* report, DWORD len, BatteryState& out) {
    if (len < 9 || report[0] != REPORT_ID) return ParseResult::MISMATCH;
    const BYTE* b = report + 1;
    const USHORT cmd = (USHORT)(b[3] | (b[4] << 8));
    if (cmd == 0x0000 && b[1] == 0 && b[2] == 0) return ParseResult::REJECTED;
    if (cmd != (USHORT)(CMD_LO | (CMD_HI << 8))) return ParseResult::MISMATCH;
    const int dlen = b[2];
    if (dlen < 0 || 7 + dlen >= 63) return ParseResult::MISMATCH;
    if ((b[1] & 0x01) && b[7 + dlen] != XorChecksum(b, 1, 6 + dlen))
        return ParseResult::MISMATCH;
    const BYTE* p = b + 7;
    if (dlen == 1 && p[0] == 0xFF) return ParseResult::BUSY;   // 鼠标休眠/离线
    if (dlen < 14) return ParseResult::MISMATCH;
    out.battery = p[12];
    out.charging = p[11] != 0;
    out.linked = p[10] != 0;
    return ParseResult::OK;
}

// ── HID 设备发现: 枚举 HID 接口, 找 0x3837 + usage page 0xFF01 ─────────────
struct DeviceEntry {
    HANDLE h = nullptr;
    USHORT pid = 0;
    wchar_t product[128] = L"MCHOSE";
    wchar_t link[8] = L"";
};

// ── HID 设备发现: 枚举所有 0x3837 + usage page 0xFF01 的配置通道 ────────────
// 有线直连的条目排在接收器前面 (同时在线时优先问有线)
static void OpenMchoseConfigDevices(std::vector<DeviceEntry>& out) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO dev_info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return;

    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA if_data = {};
        if_data.cbSize = sizeof(if_data);
        if (!SetupDiEnumDeviceInterfaces(dev_info, nullptr, &hid_guid, i, &if_data))
            break;   // 枚举完毕

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
            if (HidD_GetAttributes(h, &attr) && attr.VendorID == MCHOSE_VID
                && HidD_GetPreparsedData(h, &prep) && prep != nullptr) {
                HIDP_CAPS caps = {};
                if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS
                    && caps.UsagePage == CFG_USAGE_PAGE && caps.Usage == CFG_USAGE) {
                    DeviceEntry e;
                    e.h = h;
                    e.pid = attr.ProductID;
                    e.product[0] = L'\0';
                    HidD_GetProductString(h, e.product, sizeof(e.product));
                    if (e.product[0] == L'\0') wcscpy_s(e.product, L"MCHOSE");
                    wcscpy_s(e.link, IsReceiverPid(e.pid) ? L"2.4G" : L"USB");
                    if (IsReceiverPid(e.pid)) out.push_back(e);
                    else out.insert(out.begin(), e);   // 有线排最前
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
static ParseResult QueryOnce(HANDLE h, HANDLE io_event, BYTE seq, BatteryState& out) {
    BYTE frame[64];
    BuildFrame(frame, seq);

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
        BYTE report[64];
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
        // 命令号不匹配的报文(鼠标主动推送的广播)忽略, 继续读
    }
    return ParseResult::MISMATCH;
}

// ── 缓存更新 (工作线程调用) ─────────────────────────────────────────────────
static void PublishState(const BatteryState& st, LinkState link) {
    AcquireSRWLockExclusive(&g_lock);
    g_state = st;
    if (st.valid) {
        g_graph = st.battery / 100.0f;
    } else {
        g_graph = 0.0f;
    }
    switch (link) {
    case LinkState::LINKED:
        swprintf_s(g_tooltip, L"%ls (%ls)\r\n电量 %d %% · %ls%ls",
                   st.product, st.link, st.battery,
                   st.charging ? L"充电中" : L"未充电",
                   st.linked ? L"" : L" · 鼠标未连接!");
        break;
    case LinkState::BUSY:
        if (st.valid)
            swprintf_s(g_tooltip, L"%ls (%ls)\r\n电量 %d %% · 鼠标休眠中, 显示上次数据",
                       st.product, st.link, st.battery);
        else
            swprintf_s(g_tooltip, L"%ls (%ls)\r\n鼠标休眠/离线, 动一下鼠标即可",
                       st.product, st.link);
        break;
    case LinkState::REJECTED:
        wcscpy_s(g_tooltip, L"设备拒绝了电量命令 (旧款迈从鼠标不适用本插件)");
        break;
    default:
        wcscpy_s(g_tooltip, L"未找到迈从鼠标 (VID 3837 / usage page FF01)");
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// ── 工作线程: 轮询电量 ──────────────────────────────────────────────────────
static DWORD WINAPI WorkerProc(LPVOID) {
    BYTE seq = 0;
    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        std::vector<DeviceEntry> devices;
        OpenMchoseConfigDevices(devices);
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

        int sticky = -1;           // 上一轮应答的设备索引 (有线/接收器间保持)
        BatteryState last_good;   // 休眠时保留上次的真实数据
        bool device_ok = true;    // 全部 IO 失败 -> 重新枚举

        while (device_ok && WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
            // 查询顺序: 上次应答的设备优先, 其余按发现序 (有线在前)
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

            bool any_busy = false, any_reject = false, any_iofail = false;
            bool got_ok = false;
            BatteryState fresh;
            for (int n = 0; n < order_n && !got_ok; ++n) {
                DeviceEntry& dev = devices[order[n]];
                BatteryState cur;
                wcscpy_s(cur.product, dev.product);
                wcscpy_s(cur.link, dev.link);
                ParseResult r = ParseResult::MISMATCH;
                for (int attempt = 0; attempt < BUSY_ATTEMPTS; ++attempt) {
                    r = QueryOnce(dev.h, io_event, seq++, cur);
                    if (r != ParseResult::BUSY) break;
                    if (WaitForSingleObject(g_stop_event, 100) != WAIT_TIMEOUT) break;
                }
                switch (r) {
                case ParseResult::OK:
                    cur.valid = true;
                    fresh = cur;
                    last_good = cur;
                    sticky = order[n];
                    got_ok = true;
                    break;
                case ParseResult::BUSY:    any_busy = true; break;
                case ParseResult::REJECTED: any_reject = true; break;
                default:                    any_iofail = true; break;
                }
            }

            if (got_ok) {
                g_link_state = LinkState::LINKED;
                PublishState(fresh, LinkState::LINKED);
                if (WaitForSingleObject(g_stop_event, POLL_OK_MS) != WAIT_TIMEOUT)
                    break;
            } else if (any_busy) {
                // 鼠标休眠: 有上次数据就继续显示, 否则提示动一下鼠标
                g_link_state = LinkState::BUSY;
                if (last_good.valid)
                    PublishState(last_good, LinkState::BUSY);
                else {
                    BatteryState hint;
                    wcscpy_s(hint.product, devices[0].product);
                    wcscpy_s(hint.link, devices[0].link);
                    PublishState(hint, LinkState::BUSY);
                }
                if (WaitForSingleObject(g_stop_event, POLL_RETRY_MS) != WAIT_TIMEOUT)
                    break;
            } else if (any_reject) {
                g_link_state = LinkState::REJECTED;
                BatteryState hint;
                wcscpy_s(hint.product, devices[0].product);
                wcscpy_s(hint.link, devices[0].link);
                PublishState(hint, LinkState::REJECTED);
                if (WaitForSingleObject(g_stop_event, POLL_OK_MS) != WAIT_TIMEOUT)
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
    const wchar_t* GetItemName() const override { return L"鼠标电量"; }
    const wchar_t* GetItemId() const override { return L"mchose_mouse_battery"; }
    const wchar_t* GetItemLableText() const override { return L"鼠标: "; }
    const wchar_t* GetItemValueText() const override {
        // 按 TM 推送的"数值和单位使用空格分隔"设置动态格式化
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
        // 样例文本决定数值区宽度, 跟随任务栏的空格设置
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
class CMchosePlugin : public ITMPlugin {
public:
    IPluginItem* GetItem(int index) override { return index == 0 ? &m_item : nullptr; }
    void DataRequired() override {}   // 数据由后台线程维护, 这里无需做事

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME:        return L"MCHOSE 鼠标电量";
        case TMI_DESCRIPTION: return L"显示迈从 (MCHOSE) V3 世代无线鼠标的电量, "
                                    L"支持 2.4G 接收器与 USB 有线, 直接 HID 通信无需官方驱动。";
        case TMI_AUTHOR:      return L"reverse-engineered with ZCode";
        case TMI_COPYRIGHT:   return L"MIT";
        case TMI_VERSION:     return L"1.0";
        case TMI_URL:         return L"https://github.com/OpenMouse-Project/mouse-protocol";
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
        // 只解析我们关心的索引 (EI_CONFIG_DIR 等传的是路径, 不在此列)
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

static CMchosePlugin g_plugin;

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &g_plugin;
}

static void StopWorker() {
    if (!g_running.exchange(false)) return;
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_thread) {
        if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0) {
            // 读取单次最长 ~1.2s 且分片可中断, 5s 足够; 极端情况强制结束
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
