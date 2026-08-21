/* The palette.
 *
 * borealis draws in Horizon's colours, which is the right default for something
 * meant to pass for a system application. This is not that: it is one client of a
 * server whose web panel, README cover and 3DS build all already share a palette,
 * and a console screen that looked like nothing else in the project would be the
 * odd one out rather than the native one.
 *
 * Every value below is a token out of server/internal/web/static/style.css. Coral is
 * the accent because that is the colour the web panel gives the Switch; the 3DS
 * build uses the blue for the same reason. Two consoles, two accents, one palette -
 * which is also how you can tell at a glance which one a screenshot came from.
 */
#include "ui/dm_ui.hpp"

namespace dm
{

/* --bg, --surf, --surf2, --bd, --tx, --mut, --sw from the stylesheet. */
static const NVGcolor BG     = nvgRGB(0x17, 0x18, 0x1b);
static const NVGcolor SURF   = nvgRGB(0x1e, 0x20, 0x24);
static const NVGcolor SURF2  = nvgRGB(0x25, 0x28, 0x2e);
static const NVGcolor BORDER = nvgRGB(0x33, 0x37, 0x3f);
static const NVGcolor TEXT   = nvgRGB(0xed, 0xee, 0xf0);
static const NVGcolor MUTED  = nvgRGB(0x8f, 0x95, 0x9f);
static const NVGcolor ACCENT = nvgRGB(0xf0, 0x73, 0x6f);
/* The accent lifted, for the far end of the focus ring's gradient. */
static const NVGcolor ACCENT_HI = nvgRGB(0xff, 0xa8, 0x9e);

void applyTheme()
{
    brls::Theme& dark = brls::Theme::getDarkTheme();

    dark.addColor("brls/clear", BG);
    dark.addColor("brls/background", BG);
    dark.addColor("brls/text", TEXT);
    dark.addColor("brls/text_disabled", MUTED);
    dark.addColor("brls/accent", ACCENT);
    dark.addColor("brls/backdrop", nvgRGBA(0x0d, 0x0e, 0x10, 0xd0));
    dark.addColor("brls/click_pulse", nvgRGBA(0xf0, 0x73, 0x6f, 0x30));

    /* The ring borealis animates around whatever has focus. It is the one piece of
     * motion in the interface that says where you are, so it gets the accent
     * rather than a neutral. */
    dark.addColor("brls/highlight/background", SURF2);
    dark.addColor("brls/highlight/color1", ACCENT);
    dark.addColor("brls/highlight/color2", ACCENT_HI);

    dark.addColor("brls/applet_frame/separator", BORDER);

    dark.addColor("brls/sidebar/background", SURF);
    dark.addColor("brls/sidebar/active_item", ACCENT);
    dark.addColor("brls/sidebar/separator", BORDER);

    dark.addColor("brls/header/border", BORDER);
    dark.addColor("brls/header/rectangle", ACCENT);
    dark.addColor("brls/header/subtitle", MUTED);

    /* A dialog's buttons. The primary one is the accent with the background's own
     * colour written on it, which is the same relationship the web panel's key
     * button has. */
    dark.addColor("brls/button/primary_enabled_background", ACCENT);
    dark.addColor("brls/button/primary_disabled_background", SURF2);
    dark.addColor("brls/button/primary_enabled_text", BG);
    dark.addColor("brls/button/primary_disabled_text", MUTED);

    dark.addColor("brls/button/default_enabled_background", SURF2);
    dark.addColor("brls/button/default_disabled_background", SURF);
    dark.addColor("brls/button/default_enabled_text", TEXT);
    dark.addColor("brls/button/default_disabled_text", MUTED);

    dark.addColor("brls/button/highlight_enabled_text", ACCENT);
    dark.addColor("brls/button/highlight_disabled_text", MUTED);

    dark.addColor("brls/button/enabled_border_color", BORDER);
    dark.addColor("brls/button/disabled_border_color", BORDER);

    dark.addColor("brls/list/listItem_value_color", ACCENT);

    dark.addColor("brls/slider/pointer_color", TEXT);
    dark.addColor("brls/slider/pointer_border_color", BORDER);
    dark.addColor("brls/slider/line_filled", ACCENT);
    dark.addColor("brls/slider/line_empty", BORDER);

    dark.addColor("brls/spinner/bar_color", nvgRGBA(0xf0, 0x73, 0x6f, 0x60));

    /* Custom, used by the views in this build. */
    dark.addColor("daemoon/panel", SURF);
    dark.addColor("daemoon/tile", SURF2);
    dark.addColor("daemoon/border", BORDER);
    dark.addColor("daemoon/muted", MUTED);
    dark.addColor("daemoon/accent", ACCENT);
}

} // namespace dm
