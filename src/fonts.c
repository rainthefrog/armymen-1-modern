/* ============================================================================================
 * fonts.c - font substitution for the game's menu text
 *
 * The game creates its menu fonts (FUN_00413800 -> FUN_00413d30 -> CreateFontA) with the face
 * name "ArialNarrow" (no space). The real font is "Arial Narrow", which ships with Microsoft
 * Office, not with Windows. Without it Windows falls back to a wider face, so labels such as
 * "BACK" were too wide for the button pictures, which were drawn for Arial Narrow.
 *
 * font_CreateFontA replaces CreateFontA for Armymen.exe (import hook, patches.c):
 *   - "ArialNarrow" -> "Arial Narrow" if that font is installed
 *   - otherwise Arial, squeezed to Arial Narrow's width (NARROW_RATIO of Arial's average width)
 * Every font the game asks for is logged once ([debug] log=1).
 * ============================================================================================ */
#include "common.h"

#define NARROW_RATIO 0.82   /* Arial Narrow's average character width relative to Arial's */
#define SMALL_FONT   12     /* fonts this small (|height|) keep Arial's own width */

static int CALLBACK found_font(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM found)
{
    *(int *)found = 1;
    return 0;
}

static int font_installed(const char *face)
{
    LOGFONTA lf = { .lfCharSet = DEFAULT_CHARSET };
    lstrcpynA(lf.lfFaceName, face, LF_FACESIZE);
    HDC dc = GetDC(NULL);
    int found = 0;
    EnumFontFamiliesExA(dc, &lf, found_font, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

/* Average character width Arial would get at this height, narrowed. */
static int narrow_width(int height, int weight)
{
    HFONT f = CreateFontA(height, 0, 0, 0, weight, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "Arial");
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICA tm;
    int w = GetTextMetricsA(dc, &tm) ? (int)(tm.tmAveCharWidth * NARROW_RATIO + 0.5) : 0;
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    DeleteObject(f);
    return w;
}

HFONT WINAPI font_CreateFontA(int height, int width, int escapement, int orientation, int weight,
                              DWORD italic, DWORD underline, DWORD strikeout, DWORD charset,
                              DWORD out_precision, DWORD clip_precision, DWORD quality,
                              DWORD pitch_family, LPCSTR face)
{
    const char *asked = face ? face : "(default)";
    if (face && !lstrcmpiA(face, "ArialNarrow")) {
        static int have_narrow = -1;
        if (have_narrow < 0) have_narrow = font_installed("Arial Narrow");
        if (have_narrow) {
            face = "Arial Narrow";
        } else {
            face = "Arial";
            /* Squeeze only the menu fonts (14/20/36). The HUD message font (height -10, game
             * FONT_DASHCOMM) squeezed to 3 px average width lost glyph detail on Windows ("t" drew
             * as "l"); the game wraps that text by measuring it, so plain Arial fits. */
            if (width == 0 && (height > SMALL_FONT || height < -SMALL_FONT))
                width = narrow_width(height, weight);
        }
    }
    static int logged;
    if (logged++ < 16)
        mlog("font: game asked for \"%s\" height %d weight %d -> using \"%s\" width %d", asked, height, weight,
             face ? face : "(default)", width);
    /* no anti-aliasing (2026-11-17): the menus redraw a button's text in its hover colour over the old text;
     * soft edges of both colours mixed into a smear. Hard edges cover exactly the same pixels each time. */
    quality = NONANTIALIASED_QUALITY;
    return CreateFontA(height, width, escapement, orientation, weight, italic, underline, strikeout, charset,
                       out_precision, clip_precision, quality, pitch_family, face);
}
