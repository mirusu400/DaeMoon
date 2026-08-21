/* One save in the grid.
 *
 * An icon and a name, and nothing else: the tile answers "which game is this" and
 * the panel beside it answers everything else. That split is what makes a grid
 * readable at a glance where a table of the same facts is not.
 *
 * The icon is set after the tile is built rather than in the constructor. Reading it
 * costs a 144 KiB record out of the ns service per title, and doing sixty of those
 * before the first frame is a startup that looks like a hang. So the grid appears
 * immediately with plain tiles and the pictures land in it as they are read.
 */
#include "ui/dm_ui.hpp"

namespace dm
{

/* 1280 by 720 is the design space borealis lays out in, so these are the real
 * numbers rather than a scale factor. Five columns is what fits beside a panel wide
 * enough for a game's full name. */
static constexpr float TILE_W    = 150.0f;
static constexpr float ICON_SIZE = 150.0f;
static constexpr float TILE_H    = 208.0f;

SaveTile::SaveTile(MainScreen* screen, size_t index)
    : screen(screen)
    , index(index)
{
    this->setAxis(brls::Axis::COLUMN);
    this->setWidth(TILE_W);
    this->setHeight(TILE_H);
    this->setMargins(0.0f, 18.0f, 18.0f, 0.0f);
    this->setFocusable(true);
    this->setCornerRadius(6.0f);

    /* Underneath the icon, and visible while there is none. A title whose game has
     * been deleted still has a save worth syncing and nothing to draw for it, so an
     * empty square keeps the grid aligned where a gap would pull the row apart. */
    blank = new brls::Box();
    blank->setWidth(ICON_SIZE);
    blank->setHeight(ICON_SIZE);
    blank->setCornerRadius(6.0f);
    blank->setBackgroundColor(brls::Application::getTheme()["daemoon/tile"]);
    this->addView(blank);

    icon = new brls::Image();
    icon->setWidth(ICON_SIZE);
    icon->setHeight(ICON_SIZE);
    icon->setCornerRadius(6.0f);
    icon->setScalingType(brls::ImageScalingType::FILL);
    icon->setVisibility(brls::Visibility::GONE);
    this->addView(icon);

    name = new brls::Label();
    name->setText(std::string(app().titles[index].name));
    name->setFontSize(18.0f);
    name->setLineHeight(1.2f);
    name->setMarginTop(8.0f);
    name->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    name->setTextColor(brls::Application::getTheme()["brls/text"]);
    this->addView(name);
}

void SaveTile::setIcon(const unsigned char* jpeg, size_t len)
{
    if (jpeg == nullptr || len == 0)
        return;

    icon->setImageFromMem(jpeg, (int)len);
    icon->setVisibility(brls::Visibility::VISIBLE);
    /* Both were in the layout so that the picture appearing does not move the label
     * under it. Once there is a picture the placeholder is not just invisible, it is
     * out of the layout, or the tile would be two squares tall. */
    blank->setVisibility(brls::Visibility::GONE);
}

void SaveTile::onFocusGained()
{
    Box::onFocusGained();

    /* The panel follows the cursor rather than a press. Reading what a save is
     * before deciding to touch it is the whole reason the panel is there. */
    screen->onTileFocused(index);
}

} // namespace dm
