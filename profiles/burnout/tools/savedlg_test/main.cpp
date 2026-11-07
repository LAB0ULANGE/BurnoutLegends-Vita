// Test: can a homebrew open the Vita's native save data dialog?
// Creates one slot ("Profil 1 (test)") through SceAppUtil and opens the
// list dialog in load mode. Every return code goes to
// ux0:data/savedlg_test.log. The dialog's structures are not in vitasdk:
// declared here after Vita3K's (vita3k/dialog/include/dialog/types.h).
#include <vitaGL.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>

#include <cstdio>
#include <cstring>

namespace {

struct SaveDataDialogListParam {
    const SceAppUtilSaveDataSlot *slotList;
    SceUInt32 slotListSize;
    SceInt32 focusPos;  // 0 list head
    SceAppUtilSaveDataSlotId focusId;
    const char *listTitle;
    SceInt32 itemStyle;  // 0 title, date, subtitle
    char reserved[28];
};

struct SaveDataDialogParam {
    SceUInt32 sdkVersion;
    SceCommonDialogParam commonParam;
    SceInt32 mode;      // 2 list
    SceInt32 dispType;  // 1 save, 2 load
    void *fixedParam;
    SaveDataDialogListParam *listParam;
    void *userMsgParam;
    void *sysMsgParam;
    void *errorCodeParam;
    void *progressBarParam;
    void *slotConfParam;
    SceInt32 flag;
    void *userdata;
    char reserved[32];
};

struct SaveDataDialogResult {
    SceInt32 mode;
    SceInt32 result;  // 0 ok, 1 cancelled, 2 aborted
    SceInt32 buttonId;
    SceInt32 slotId;
    void *slotInfo;
    void *userdata;
    char reserved[32];
};

std::FILE *g_log;

void log_line(const char *what, int value) {
    std::fprintf(g_log, "%s: 0x%08X (%d)\n", what, static_cast<unsigned>(value), value);
    std::fflush(g_log);
}

void wide(SceWChar16 *out, int size, const char *text) {
    int i = 0;
    for (; text[i] != '\0' && i < size - 1; ++i) out[i] = static_cast<SceWChar16>(text[i]);
    out[i] = 0;
}

} // namespace

extern "C" {
int sceSaveDataDialogInit(SaveDataDialogParam *param);
int sceSaveDataDialogGetStatus(void);
int sceSaveDataDialogGetResult(SaveDataDialogResult *result);
int sceSaveDataDialogTerm(void);
}

int main() {
    g_log = std::fopen("ux0:data/savedlg_test.log", "w");
    if (g_log == nullptr) sceKernelExitProcess(0);
    std::fprintf(g_log, "struct sizes: list %u param %u result %u common %u slot %u slotparam %u\n",
                 static_cast<unsigned>(sizeof(SaveDataDialogListParam)), static_cast<unsigned>(sizeof(SaveDataDialogParam)),
                 static_cast<unsigned>(sizeof(SaveDataDialogResult)), static_cast<unsigned>(sizeof(SceCommonDialogParam)),
                 static_cast<unsigned>(sizeof(SceAppUtilSaveDataSlot)), static_cast<unsigned>(sizeof(SceAppUtilSaveDataSlotParam)));

    vglInit(0x800000);
    log_line("vglInit", 0);

    SceAppUtilInitParam init{};
    SceAppUtilBootParam boot{};
    log_line("sceAppUtilInit", sceAppUtilInit(&init, &boot));

    SceAppUtilSaveDataSlotParam slot_param{};
    wide(slot_param.title, 32, "Profil 1 (test)");
    wide(slot_param.subTitle, 64, "Burnout Legends");
    wide(slot_param.detail, 256, "Test de la fenetre de sauvegarde native");
    sceRtcGetCurrentClockLocalTime(&slot_param.modifiedTime);
    log_line("sceAppUtilSaveDataSlotCreate(0)", sceAppUtilSaveDataSlotCreate(0, &slot_param, nullptr));
    SceAppUtilSaveDataSlotParam check{};
    log_line("sceAppUtilSaveDataSlotGetParam(0)", sceAppUtilSaveDataSlotGetParam(0, &check, nullptr));

    SceAppUtilSaveDataSlot slots[4]{};
    for (unsigned i = 0; i < 4; ++i) slots[i].id = i;
    SaveDataDialogListParam list{};
    list.slotList = slots;
    list.slotListSize = 4;
    list.listTitle = "Choisir un profil";
    SaveDataDialogParam param{};
    param.sdkVersion = PSP2_SDK_VERSION;
    _sceCommonDialogSetMagicNumber(&param.commonParam);
    param.mode = 2;
    param.dispType = 2;
    param.listParam = &list;
    log_line("sceSaveDataDialogInit", sceSaveDataDialogInit(&param));

    // Up to 60 s: the dialog draws itself over the frames vitaGL swaps.
    int status = 0;
    for (int frame = 0; frame < 3600; ++frame) {
        glClearColor(0.05f, 0.1f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        status = sceSaveDataDialogGetStatus();
        if (status == 2) break;  // finished
        vglSwapBuffers(GL_TRUE);
    }
    log_line("final status", status);
    SaveDataDialogResult result{};
    log_line("sceSaveDataDialogGetResult", sceSaveDataDialogGetResult(&result));
    log_line("  result (0 ok, 1 cancel)", result.result);
    log_line("  slotId", result.slotId);
    log_line("  buttonId", result.buttonId);
    log_line("sceSaveDataDialogTerm", sceSaveDataDialogTerm());
    std::fclose(g_log);

    sceKernelExitProcess(0);
    return 0;
}
