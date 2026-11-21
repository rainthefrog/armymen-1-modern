/* ============================================================================================
 * screenbtn.c - menu / mission / death screen buttons show their pressed look before they act
 *
 * 2026-11-19, user: "when hitting the mission/death screens - the buttons don't depress when hit. just
 * immediately moving to next screen".
 * The game acts on mouse DOWN: InTitleScreenLButtonDn (game binary; exe 0x45c490) -> SBtnSetState(button,
 * BUTTON_CLICKED) (0x45e370, __fastcall ecx = button, edx = state; game binary), which stores the state
 * (+0x04), sets bUpdate (+0x3c) and runs SBtnExecuteCallback (0x45eb30, same convention) at once. A callback
 * that changes the screen (CONTINUE, Play Again YES / NO, ...) deletes the old screen before anything is drawn,
 * so the pressed picture (BTN64wDn.bmp ...) never shows - in the original too.
 *
 * Wrapped SBtnSetState: while the game handles WM_LBUTTONDOWN (ddraw.c wndproc_hook sets the flag) a CLICKED
 * state is stored with SBTN_CLICK_BYPASS (attributes +0x00 bit 0x800: SBtnExecuteCallback then skips the
 * CLICKED callback), so the button is drawn pressed; PRESS_MS later a window timer (game thread) runs the
 * callback - if the button still exists (button list 0x6a5058, count 0x6a546c) and is still CLICKED. The bypass
 * bit stays set until then: the game's per-frame SBtnUpdate also runs the current state's callback (Wine: with
 * the bit cleared at once the screens still changed before the timer).
 * Clicks while one is pending are ignored. Buttons that go to EDIT (name entry, key binding) are unchanged.
 * Keyboard (Enter) still acts at once (it calls the callback directly).
 * [display] button_press = 1
 * ============================================================================================ */
#include "common.h"

#define BTN_LIST        ((BYTE **)0x6a5058)
#define BTN_COUNT       (*(int *)0x6a546c)
#define BTN_ATTR(b)     (*(DWORD *)(b))
#define BTN_STATE(b)    (*(int *)((b) + 0x04))
#define BTN_CLICKED_CB(b) (*(void **)((b) + 0x30))
#define BUTTON_CLICKED  2
#define CLICK_BYPASS    0x800
#define EXECUTE_CALLBACK ((SBtnStateFn)0x45eb30)
#define PRESS_MS        140
#define PRESS_TIMER     0x4d42

SBtnStateFn screenbtn_set_state_original;
static int g_in_lbuttondown;
static BYTE *g_pending;
static DWORD g_pending_at;
static HWND g_hwnd;

void screenbtn_lbuttondown(HWND h, int inside) { g_in_lbuttondown = inside; g_hwnd = h; }

int __fastcall screenbtn_set_state(BYTE *btn, int state)
{
    /* only on the full-screen menus: over a mission (the mission-over radio, briefings) the game runs its own loop and
     * the release / timer may never reach us - the held CONTINUE never fired and the game stayed locked (2026-11-20,
     * user: "mission 2 freeze is repeatable. the voice clip plays and it stays locked") */
    if (!g_cfg.button_press || !g_in_lbuttondown || state != BUTTON_CLICKED || !btn || !g_hwnd || layout_view_is_gameplay())
        return screenbtn_set_state_original(btn, state);
    if (g_pending) return 0;                                /* one press at a time */
    DWORD had = BTN_ATTR(btn) & CLICK_BYPASS;
    BTN_ATTR(btn) |= CLICK_BYPASS;
    int r = screenbtn_set_state_original(btn, state);       /* stored + drawn pressed, callback held back */
    if (had) return r;
    if (BTN_STATE(btn) == BUTTON_CLICKED && BTN_CLICKED_CB(btn)) {
        g_pending = btn; g_pending_at = GetTickCount();     /* the bypass stays until then: the game's own
                                                               per-frame SBtnUpdate runs the state's callback too */
        SetTimer(g_hwnd, PRESS_TIMER, PRESS_MS, NULL);
    } else BTN_ATTR(btn) &= ~CLICK_BYPASS;
    return r;
}

static int still_there(BYTE *btn)
{
    int n = BTN_COUNT;
    for (int i = 0; i < n && i < 256; i++) if (BTN_LIST[i] == btn) return 1;
    return 0;
}

static void fire_pending(HWND h, const char *why)
{
    KillTimer(h, PRESS_TIMER);
    BYTE *btn = g_pending;
    g_pending = NULL;
    if (btn && still_there(btn)) {
        BTN_ATTR(btn) &= ~CLICK_BYPASS;
        if (BTN_STATE(btn) == BUTTON_CLICKED) {
            if (g_cfg.log >= 1) mlog("screenbtn: pressed button %p acts now (%s, %lu ms after the press)", btn, why, GetTickCount() - g_pending_at);
            EXECUTE_CALLBACK(btn, BUTTON_CLICKED);
            return;
        }
    }
    if (btn) mlog("screenbtn: pressed button %p gone before its action", btn);
}

/* ddraw.c wndproc_hook, EVERY message, before anything else: 1 = ours (handled).
 * The held-back action runs on the button's RELEASE (once RELEASE_MIN_MS have passed, so the pressed picture
 * had a frame), else with the first message after PRESS_MS, else by the timer. 2026-11-19, user: "the mission
 * complete doesn't allow skipping now like it did before" - with the timer alone the action waited for a
 * WM_TIMER, the lowest-priority message: while the game runs its own loop during the mission-over radio speech
 * it is not handed out, so CONTINUE acted only when the speech had ended. Mouse messages do arrive there. */
#define RELEASE_MIN_MS 40
int screenbtn_message(HWND h, UINT msg, WPARAM wp)
{
    if (msg == WM_TIMER && wp == PRESS_TIMER) { if (g_pending) fire_pending(h, "timer"); else KillTimer(h, PRESS_TIMER); return 1; }
    if (!g_pending) return 0;
    DWORD waited = GetTickCount() - g_pending_at;
    if (msg == WM_LBUTTONUP && waited >= RELEASE_MIN_MS) fire_pending(h, "release");
    else if (waited >= PRESS_MS) fire_pending(h, "next message");
    return 0;
}
