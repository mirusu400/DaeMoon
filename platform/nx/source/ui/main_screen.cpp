/* The screen: a grid of saves, and what is known about the one under the cursor.
 *
 * The shape is the 3DS build's, because it was arrived at there for reasons that did
 * not change on the way over: pictures to find a game by, one panel that says
 * everything else about the one you are on, and the destructive action nowhere near
 * the button that backs a save up.
 *
 * That last one is why the self test is a labelled button inside the panel rather
 * than an action on the grid. It used to be X with a shoulder held, which was
 * undocumented until a legend was written for it and still one press away from a real
 * save. Here it takes moving the cursor off the grid, onto a button that says what it
 * does, and then answering twice.
 */
#include "ui/dm_ui.hpp"

#include <cstdlib>
#include <thread>

namespace dm
{

/* Five columns, 150 wide with 18 between them, is 822 - which leaves a panel wide
 * enough for a game's full name at a readable size on a 1280 wide layout. */
static constexpr int   GRID_COLUMNS = 5;
static constexpr float PANEL_WIDTH  = 400.0f;

MainScreen::MainScreen()
{
    this->setAxis(brls::Axis::ROW);
    this->setGrow(1.0f);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);

    grid = new brls::Box(brls::Axis::COLUMN);
    grid->setPadding(30.0f, 20.0f, 30.0f, 40.0f);
    scroll->setContentView(grid);
    this->addView(scroll);

    details = new brls::Box(brls::Axis::COLUMN);
    details->setWidth(PANEL_WIDTH);
    details->setPadding(30.0f, 30.0f, 30.0f, 30.0f);
    details->setBackgroundColor(brls::Application::getTheme()["daemoon/panel"]);
    this->addView(details);

    /* Which account these saves belong to, said on screen rather than assumed. A
     * different account is a different save, and this is the only thing on the
     * screen that says which one is being looked at. */
    auto* accountLabel = new brls::Label();
    accountLabel->setText(str(DAEMOON_STR_NX_ACCOUNT) + ": " +
                          std::string(app().save.account.nickname));
    accountLabel->setFontSize(17.0f);
    accountLabel->setTextColor(brls::Application::getTheme()["daemoon/muted"]);
    details->addView(accountLabel);

    auto* serverLabel = new brls::Label();
    serverLabel->setText(str(DAEMOON_STR_SETTINGS_SERVER) + ": " +
                         (app().config.server_url[0] != '\0'
                              ? std::string(app().config.server_url)
                              : str(DAEMOON_STR_SETTINGS_UNSET)));
    serverLabel->setFontSize(17.0f);
    serverLabel->setMarginBottom(24.0f);
    serverLabel->setTextColor(brls::Application::getTheme()["daemoon/muted"]);
    details->addView(serverLabel);

    detName = new brls::Label();
    detName->setFontSize(26.0f);
    detName->setLineHeight(1.25f);
    detName->setMarginBottom(10.0f);
    details->addView(detName);

    detId = new brls::Label();
    detId->setFontSize(18.0f);
    detId->setTextColor(brls::Application::getTheme()["daemoon/muted"]);
    details->addView(detId);

    detNote = new brls::Label();
    detNote->setFontSize(18.0f);
    detNote->setMarginTop(6.0f);
    detNote->setTextColor(brls::Application::getTheme()["daemoon/muted"]);
    details->addView(detNote);

    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    details->addView(spacer);

    /* The one destructive thing this build can do, behind a label that says so. It
     * clears the save it is run against to prove that clearing works, so it is not
     * an action on the grid and it is not next to anything else. */
    auto* selfTest = new brls::Button();
    selfTest->setStyle(&brls::BUTTONSTYLE_BORDERED);
    selfTest->setText(str(DAEMOON_STR_MENU_SELFTEST));
    selfTest->setFontSize(18.0f);
    selfTest->registerClickAction([this](brls::View*) {
        if (app().count == 0 || operationRunning())
            return true;
        runSelfTest(selected);
        return true;
    });
    details->addView(selfTest);

    /* The actions the footer draws hints for. Registered on the screen rather than
     * on each tile so that the hints do not disappear when focus moves into the
     * panel, and so there is one place that decides what a button does. */
    this->registerAction(str(DAEMOON_STR_MENU_BACKUP), brls::ControllerButton::BUTTON_A,
                         [this](brls::View*) {
                             if (app().count == 0 || operationRunning())
                                 return true;
                             runBackup(selected);
                             return true;
                         });

    this->registerAction(str(DAEMOON_STR_MENU_SYNC), brls::ControllerButton::BUTTON_Y,
                         [this](brls::View*) {
                             if (app().count == 0 || operationRunning())
                                 return true;
                             runSync(selected);
                             return true;
                         });

    this->registerAction(str(DAEMOON_STR_NX_RELOAD), brls::ControllerButton::BUTTON_X,
                         [this](brls::View*) {
                             if (operationRunning())
                                 return true;
                             this->rebuild();
                             return true;
                         });

    this->rebuild();
}

void MainScreen::rebuild()
{
    daemoon_result_t r = app().reloadTitles();

    if (r != DAEMOON_OK)
        brls::Application::notify(str(DAEMOON_STR_ERR_TITLE_LIST));

    /* Anything already in flight for the previous list is now aimed at views that
     * are about to be deleted. The generation is what the icon thread checks before
     * it touches a tile. */
    generation++;
    tiles.clear();
    grid->clearViews();
    emptyMsg = nullptr;

    if (selected >= app().count)
        selected = app().count > 0 ? app().count - 1 : 0;

    buildGrid();
    showDetails(selected);

    if (app().count > 0)
    {
        /* Not on the first pass: this runs from the constructor, where there is no
         * tree above this view yet and nothing to hand focus back to. borealis picks
         * the first focusable child on its own in that case, which is the same tile. */
        if (attached)
            brls::Application::giveFocus(tiles[selected]);
        loadIconsInBackground();
    }
    attached = true;
}

void MainScreen::buildGrid()
{
    if (app().count == 0)
    {
        emptyMsg = new brls::Label();
        emptyMsg->setText(str(DAEMOON_STR_GRID_EMPTY));
        emptyMsg->setFontSize(22.0f);
        emptyMsg->setTextColor(brls::Application::getTheme()["daemoon/muted"]);
        grid->addView(emptyMsg);
        return;
    }

    /* Rows built by hand because the layout has no wrap. That is not a workaround:
     * the column count is a decision about how wide a tile has to be to recognise a
     * game, and having it written once here is better than letting it fall out of
     * whatever width the panel happens to leave. */
    brls::Box* row = nullptr;

    for (size_t i = 0; i < app().count; ++i)
    {
        if (i % GRID_COLUMNS == 0)
        {
            row = new brls::Box(brls::Axis::ROW);
            grid->addView(row);
        }

        auto* tile = new SaveTile(this, i);
        row->addView(tile);
        tiles.push_back(tile);
    }
}

void MainScreen::onTileFocused(size_t index)
{
    selected = index;
    showDetails(index);
}

void MainScreen::showDetails(size_t index)
{
    if (app().count == 0 || index >= app().count)
    {
        detName->setText(str(DAEMOON_STR_GRID_NOTHING_SELECTED));
        detId->setText("");
        detNote->setText("");
        return;
    }

    const daemoon_title_t& t = app().titles[index];

    detName->setText(std::string(t.name));
    detId->setText(std::string(t.id));

    /* The size the backend could say cheaply, when it could. A save whose size is
     * not known is normal rather than an error - the reader reports what it has -
     * and the line is left off rather than showing a zero that means "unknown". */
    if (t.size_hint > 0)
    {
        char bytes[32];

        daemoon_fmt_bytes(bytes, sizeof(bytes), t.size_hint);
        detNote->setText(std::string(bytes));
    }
    else
    {
        detNote->setText("");
    }
}

void MainScreen::loadIconsInBackground()
{
    unsigned wanted = generation.load();

    /* The ids are copied out before the thread starts rather than read through
     * app().titles inside it. That array is freed and replaced by a reload, and a
     * generation checked one line earlier does not stop a rebuild from happening on
     * the next - the copy is what makes the thread touch nothing that can move. */
    std::vector<unsigned long long> ids;

    ids.reserve(app().count);
    for (size_t i = 0; i < app().count; ++i)
    {
        unsigned long long appId = 0;

        (void)daemoon_nx_parse_title_id(app().titles[i].id, &appId);
        ids.push_back(appId);
    }

    /* Detached because there is nothing to wait for: every icon it reads is handed
     * to the main thread, and if the list is rebuilt underneath it the generation
     * check drops what it was carrying. Sixty of these reads is around a second,
     * which is a second of the grid being usable rather than a second of black. */
    std::thread([this, wanted, ids]() {
        for (size_t i = 0; i < ids.size(); ++i)
        {
            unsigned char* jpeg = nullptr;
            size_t len = 0;

            if (generation.load() != wanted)
                return;
            if (ids[i] == 0)
                continue;
            if (daemoon_nx_icon_load(ids[i], &jpeg, &len) != DAEMOON_OK)
                continue;

            /* The picture crosses to the main thread, because borealis is not
             * thread safe and uploading a texture is drawing. The lambda owns the
             * bytes from here and frees them either way - a tile that has gone
             * still leaves a buffer behind. */
            brls::sync([this, wanted, i, jpeg, len]() {
                if (generation.load() == wanted && i < tiles.size())
                    tiles[i]->setIcon(jpeg, len);
                free(jpeg);
            });
        }
    }).detach();
}

/* --------------------------------------------------------------------- activity */

brls::View* MainActivity::createContentView()
{
    screen = new MainScreen();

    auto* frame = new brls::AppletFrame(screen);
    frame->setTitle(str(DAEMOON_STR_APP_TITLE));
    frame->setIcon(BRLS_ASSET("icon/icon.jpg"));

    /* Re-read after anything that could have changed what is on the console. A sync
     * can pull a save down and the self test empties one, and a list that still says
     * what was true before either is worse than no list. */
    setTitlesChangedCallback([this]() { screen->rebuild(); });

    return frame;
}

} // namespace dm
