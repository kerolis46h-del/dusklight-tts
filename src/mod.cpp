// Text-to-Speech for Dusklight (Twilight Princess). Windows speech engine (SAPI).
// Speaks each dialogue page once it has been laid out, reads choices when they appear,
// and stops when the textbox closes.

#include "global.h"
#include "d/d_msg_class.h"
#include "d/d_msg_object.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#endif

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(UiService, svc_ui);

DEFINE_HOOK(&dMsgObject_c::_draw, MsgDraw);

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static ConfigVarHandle sRateVar = 0;
static ConfigVarHandle sVolumeVar = 0;
static ConfigVarHandle sChoicesVar = 0;

static int GetInt(ConfigVarHandle var, int fallback) {
    int64_t v = fallback;
    if (svc_config->get_int(mod_ctx, var, &v) != MOD_OK) {
        return fallback;
    }
    return static_cast<int>(v);
}

static bool GetBool(ConfigVarHandle var, bool fallback) {
    bool v = fallback;
    if (svc_config->get_bool(mod_ctx, var, &v) != MOD_OK) {
        return fallback;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Speech backend (Windows SAPI)
// ---------------------------------------------------------------------------
#ifdef _WIN32
static ISpVoice* sVoice = nullptr;
static bool sComInitialized = false;

static void SpeechInit() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    sComInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return;
    }
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                reinterpret_cast<void**>(&sVoice)))) {
        sVoice = nullptr;
    }
}

static void SpeechShutdown() {
    if (sVoice != nullptr) {
        sVoice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
        sVoice->Release();
        sVoice = nullptr;
    }
    if (sComInitialized) {
        CoUninitialize();
        sComInitialized = false;
    }
}

// `text` is Windows-1252 / ASCII bytes (the game's English text).
static void SpeechSpeak(const std::string& text, bool interrupt) {
    if (sVoice == nullptr || text.empty()) {
        return;
    }
    int n = MultiByteToWideChar(1252, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 0) {
        return;
    }
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(1252, 0, text.c_str(), -1, wide.data(), n);

    int rate = GetInt(sRateVar, 0);
    int volume = GetInt(sVolumeVar, 100);
    rate = rate < -10 ? -10 : (rate > 10 ? 10 : rate);
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    sVoice->SetRate(rate);
    sVoice->SetVolume(static_cast<USHORT>(volume));

    DWORD flags = SPF_ASYNC | (interrupt ? SPF_PURGEBEFORESPEAK : 0);
    sVoice->Speak(wide.c_str(), flags, nullptr);
}

static void SpeechStop() {
    if (sVoice != nullptr) {
        sVoice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    }
}
#else
static void SpeechInit() {}
static void SpeechShutdown() {}
static void SpeechSpeak(const std::string&, bool) {}
static void SpeechStop() {}
#endif

// ---------------------------------------------------------------------------
// Text cleaning: remove inline formatting codes, keep readable characters.
// The game embeds codes like ESC "CC[ffffffff]" (color) inside the text.
// ---------------------------------------------------------------------------
static bool IsEscCode(unsigned char a, unsigned char b) {
    static const char* const kCodes[] = {"CU", "CD", "CL", "CR", "LU", "LD", "ST", "CC",
                                         "GC", "FX", "FY", "SH", "SV", "GM", "HM"};
    for (const char* code : kCodes) {
        if (code[0] == static_cast<char>(a) && code[1] == static_cast<char>(b)) {
            return true;
        }
    }
    return false;
}

static std::string CleanText(const char* raw) {
    std::string out;
    if (raw == nullptr) {
        return out;
    }
    bool lastSpace = true;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw);
    while (*p != 0) {
        unsigned char c = *p;

        if (c == 0x1B) {  // start of a formatting code
            ++p;
            if (p[0] != 0 && p[1] != 0 && IsEscCode(p[0], p[1])) {
                p += 2;
                if (*p == '[') {  // optional argument in brackets
                    while (*p != 0 && *p != ']') {
                        ++p;
                    }
                    if (*p == ']') {
                        ++p;
                    }
                }
            }
            continue;
        }

        if (c == '\n' || c == '\r' || c == ' ') {
            if (!lastSpace) {
                out += ' ';
                lastSpace = true;
            }
        } else if ((c >= 0x21 && c <= 0x7E) || c >= 0xA0) {
            out += static_cast<char>(c);
            lastSpace = false;
        }
        // other control bytes are skipped
        ++p;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Dialogue tracking
// ---------------------------------------------------------------------------
static constexpr u16 kStatusIdle = 1;
static constexpr u16 kStatusPrinting = 6;
static constexpr u16 kStatusChoice2 = 8;
static constexpr u16 kStatusChoice3 = 9;

static bool sBoxOpen = false;
static bool sPageActive = false;
static bool sPageSpoken = false;
static int sStableFrames = 0;
static std::string sLastText;
static u16 sPrevStatus = 0;

static void OnMsgDrawPost(ModContext*, void* args, void*, void*) {
    dMsgObject_c* msg = mods::arg<dMsgObject_c*>(args, 0);
    if (msg == nullptr || msg->mpRefer == nullptr) {
        return;
    }

    const u16 status = msg->getStatusLocal();

    // Box closed / idle: stop talking.
    if (status == 0 || status == kStatusIdle) {
        if (sBoxOpen) {
            SpeechStop();
        }
        sBoxOpen = false;
        sPageActive = false;
        sPageSpoken = false;
        sPrevStatus = status;
        return;
    }
    sBoxOpen = true;

    if (status == kStatusPrinting) {
        if (!sPageActive) {
            sPageActive = true;
            sPageSpoken = false;
            sStableFrames = 0;
            sLastText.clear();
        }
        if (!sPageSpoken) {
            std::string cur = CleanText(msg->mpRefer->getTextPtr());
            if (!cur.empty() && cur == sLastText) {
                if (++sStableFrames >= 3) {
                    SpeechSpeak(cur, true);
                    sPageSpoken = true;
                }
            } else {
                sLastText = cur;
                sStableFrames = 0;
            }
        }
    } else {
        // Left the printing state before the text looked stable: speak what we have.
        if (sPageActive && !sPageSpoken) {
            std::string cur = CleanText(msg->mpRefer->getTextPtr());
            SpeechSpeak(cur, true);
            sPageSpoken = true;
        }
        sPageActive = false;

        // Entering a choice menu: read the options once, after the question.
        if ((status == kStatusChoice2 || status == kStatusChoice3) && sPrevStatus != status &&
            GetBool(sChoicesVar, true)) {
            std::string choices;
            for (int i = 0; i < 3; i++) {
                std::string opt = CleanText(msg->mpRefer->getSelTextPtr(i));
                if (!opt.empty()) {
                    if (!choices.empty()) {
                        choices += ". ";
                    }
                    choices += opt;
                }
            }
            SpeechSpeak(choices, false);
        }
    }
    sPrevStatus = status;
}

// ---------------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------------
static ModResult BuildPanel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Voice");

    UiControlDesc rate = UI_CONTROL_DESC_INIT;
    rate.kind = UI_CONTROL_NUMBER;
    rate.label = "Speed";
    rate.help_rml = "How fast the voice talks. 0 is normal.";
    rate.binding = UI_BINDING_CONFIG_VAR;
    rate.config_var = sRateVar;
    rate.min = -10;
    rate.max = 10;
    rate.step = 1;
    svc_ui->pane_add_control(mod_ctx, panel, &rate, nullptr);

    UiControlDesc volume = UI_CONTROL_DESC_INIT;
    volume.kind = UI_CONTROL_NUMBER;
    volume.label = "Volume";
    volume.help_rml = "How loud the voice is (0 to 100).";
    volume.binding = UI_BINDING_CONFIG_VAR;
    volume.config_var = sVolumeVar;
    volume.min = 0;
    volume.max = 100;
    volume.step = 5;
    svc_ui->pane_add_control(mod_ctx, panel, &volume, nullptr);

    UiControlDesc choices = UI_CONTROL_DESC_INIT;
    choices.kind = UI_CONTROL_TOGGLE;
    choices.label = "Read choices aloud";
    choices.help_rml = "Reads the answer options when a question appears.";
    choices.binding = UI_BINDING_CONFIG_VAR;
    choices.config_var = sChoicesVar;
    svc_ui->pane_add_control(mod_ctx, panel, &choices, nullptr);

    return MOD_OK;
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------
extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError*) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;

    d.name = "rate";
    d.type = CONFIG_VAR_INT;
    d.default_int = 0;
    svc_config->register_var(mod_ctx, &d, &sRateVar);

    d = CONFIG_VAR_DESC_INIT;
    d.name = "volume";
    d.type = CONFIG_VAR_INT;
    d.default_int = 100;
    svc_config->register_var(mod_ctx, &d, &sVolumeVar);

    d = CONFIG_VAR_DESC_INIT;
    d.name = "readChoices";
    d.type = CONFIG_VAR_BOOL;
    d.default_bool = true;
    svc_config->register_var(mod_ctx, &d, &sChoicesVar);

    SpeechInit();

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = BuildPanel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    ModResult hooked = mods::hook::add_post<MsgDraw>(OnMsgDrawPost);
    if (hooked != MOD_OK) {
        svc_log->info(mod_ctx, "tts: could not hook the dialogue code (wrong Dusklight version?)");
        return hooked;
    }

    svc_log->info(mod_ctx, "tts: ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    SpeechShutdown();
    return MOD_OK;
}

}
