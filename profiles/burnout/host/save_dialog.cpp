#include "save_dialog.hpp"

#if defined(__vita__)
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/rtc.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#endif

namespace burnout::save_dialog {

#if defined(__vita__)
namespace {

// The dialog's structures are not in vitasdk: laid out as in Vita3K
// (vita3k/dialog/include/dialog/types.h). Checked on the device with
// profiles/burnout/tools/savedlg_test.
struct ListParam {
    const SceAppUtilSaveDataSlot *slotList;
    SceUInt32 slotListSize;
    SceInt32 focusPos;  // 0 list head, 8 focusId
    SceAppUtilSaveDataSlotId focusId;
    const char *listTitle;
    SceInt32 itemStyle;  // 0 title, date, subtitle
    char reserved[28];
};

struct DialogParam {
    SceUInt32 sdkVersion;
    SceCommonDialogParam commonParam;
    SceInt32 mode;      // 2 list
    SceInt32 dispType;  // 1 save, 2 load
    void *fixedParam;
    ListParam *listParam;
    void *userMsgParam;
    void *sysMsgParam;
    void *errorCodeParam;
    void *progressBarParam;
    void *slotConfParam;
    SceInt32 flag;
    void *userdata;
    char reserved[32];
};

struct DialogResult {
    SceInt32 mode;
    SceInt32 result;  // 0 ok, 1 cancelled, 2 aborted
    SceInt32 buttonId;
    SceInt32 slotId;
    void *slotInfo;
    void *userdata;
    char reserved[32];
};

constexpr int kMaxSlots = 16;
constexpr int kSlotExists = static_cast<int>(0x80100640u);

std::atomic<bool> g_drawing{false};
enum class State { Idle, Choosing, Closing } g_state = State::Idle;
int g_choice = kCancelled;
SceAppUtilSaveDataSlot g_slots[kMaxSlots];
ListParam g_list;
DialogParam g_param;
SceAppUtilSaveDataSlotEmptyParam g_empty;
SceWChar16 g_empty_title[32];

void wide(SceWChar16 *out, int size, const std::string &text) {
    int i = 0;
    for (; i < static_cast<int>(text.size()) && i < size - 1; ++i) out[i] = static_cast<SceWChar16>(static_cast<unsigned char>(text[i]));
    out[i] = 0;
}

void local_time(std::time_t utc, SceDateTime &out) {
    // Ticks are microseconds since 0001-01-01; 1970-01-01 is 62135596800 s later.
    SceRtcTick tick{static_cast<SceUInt64>(utc + 62135596800LL) * 1000000ull};
    SceRtcTick local{};
    if (sceRtcConvertUtcToLocalTime(&tick, &local) < 0) local = tick;
    sceRtcSetTick(&out, &local);
}

} // namespace

extern "C" {
int sceSaveDataDialogInit(DialogParam *param);
int sceSaveDataDialogGetStatus(void);
int sceSaveDataDialogGetSubStatus(void);
int sceSaveDataDialogGetResult(DialogResult *result);
int sceSaveDataDialogFinish(const void *finish_param);
int sceSaveDataDialogTerm(void);
}

bool start(bool save, const std::vector<Slot> &slots, int focus) {
    if (g_state != State::Idle || slots.empty()) return false;
    static bool app_util = false;
    if (!app_util) {
        SceAppUtilInitParam init{};
        SceAppUtilBootParam boot{};
        const int result = sceAppUtilInit(&init, &boot);
        std::fprintf(stderr, "[save] sceAppUtilInit 0x%08X\n", static_cast<unsigned>(result));
        app_util = true;
    }
    const int count = static_cast<int>(slots.size()) < kMaxSlots ? static_cast<int>(slots.size()) : kMaxSlots;
    // The Vita's slots mirror the PSP saves: title and date, or empty.
    for (int i = 0; i < count; ++i) {
        if (!slots[i].exists) {
            sceAppUtilSaveDataSlotDelete(static_cast<unsigned>(i), nullptr);
        } else {
            SceAppUtilSaveDataSlotParam param{};
            wide(param.title, 32, slots[i].title);
            wide(param.subTitle, 64, slots[i].subtitle);
            wide(param.detail, 256, slots[i].detail);
            local_time(slots[i].modified, param.modifiedTime);
            int result = sceAppUtilSaveDataSlotCreate(static_cast<unsigned>(i), &param, nullptr);
            if (result == kSlotExists) result = sceAppUtilSaveDataSlotSetParam(static_cast<unsigned>(i), &param, nullptr);
            if (result < 0) std::fprintf(stderr, "[save] slot %d: 0x%08X\n", i, static_cast<unsigned>(result));
        }
        g_slots[i] = SceAppUtilSaveDataSlot{};
        g_slots[i].id = static_cast<SceAppUtilSaveDataSlotId>(i);
        if (save) {
            wide(g_empty_title, 32, "Nouveau profil");
            g_empty = SceAppUtilSaveDataSlotEmptyParam{};
            g_empty.title = g_empty_title;
            g_slots[i].emptyParam = &g_empty;
        }
    }
    g_list = ListParam{};
    g_list.slotList = g_slots;
    g_list.slotListSize = static_cast<SceUInt32>(count);
    g_list.focusPos = focus >= 0 && focus < count ? 8 : 0;
    g_list.focusId = static_cast<SceAppUtilSaveDataSlotId>(focus >= 0 ? focus : 0);
    g_list.listTitle = save ? "Sauvegarder le profil" : "Charger un profil";
    g_param = DialogParam{};
    g_param.sdkVersion = PSP2_SDK_VERSION;
    _sceCommonDialogSetMagicNumber(&g_param.commonParam);
    g_param.mode = 2;
    g_param.dispType = save ? 1 : 2;
    g_param.listParam = &g_list;
    const int result = sceSaveDataDialogInit(&g_param);
    std::fprintf(stderr, "[save] native dialog (%s, %d slots): 0x%08X\n", save ? "save" : "load", count, static_cast<unsigned>(result));
    if (result < 0) return false;
    g_choice = kCancelled;
    g_state = State::Choosing;
    g_drawing = true;
    return true;
}

int poll() {
    if (g_state == State::Idle) return kCancelled;
    if (g_state == State::Choosing) {
        // A choice leaves the dialog running with its sub-status finished:
        // read the result, then close it.
        const bool chosen = sceSaveDataDialogGetSubStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED;
        const bool closed = sceSaveDataDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED;
        if (!chosen && !closed) return kRunning;
        DialogResult result{};
        sceSaveDataDialogGetResult(&result);
        g_choice = result.result == 0 && result.slotId >= 0 ? result.slotId : kCancelled;
        std::fprintf(stderr, "[save] native dialog result %d slot %d\n", static_cast<int>(result.result), static_cast<int>(result.slotId));
        if (!closed) {
            char finish[40]{};  // SceSaveDataDialogFinishParam, flag 0
            sceSaveDataDialogFinish(finish);
        }
        g_state = State::Closing;
    }
    if (sceSaveDataDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return kRunning;
    sceSaveDataDialogTerm();
    g_drawing = false;
    g_state = State::Idle;
    return g_choice;
}

bool drawing() { return g_drawing.load(std::memory_order_relaxed); }

#else

bool start(bool, const std::vector<Slot> &, int) { return false; }
int poll() { return kCancelled; }
bool drawing() { return false; }

#endif

} // namespace burnout::save_dialog
