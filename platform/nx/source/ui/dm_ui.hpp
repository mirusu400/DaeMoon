/* The Switch interface, declared in one place.
 *
 * This build drew a text console until now, and the reason it did was honest: what
 * Phase 6 had to establish was that a save can be mounted, read, written and
 * committed here, and a list of lines proves that as well as a grid of icons would.
 * That question has an answer - 277 checks, 0 failures, on a console - so the
 * interface is no longer the thing standing in for the work.
 *
 * Two things the console could not do are why this is borealis rather than more
 * printf. The first is text: libnx's console draws a font of its own that covers
 * ASCII and stops, so a Korean console showed rubbish and the build fell back to
 * English to stay readable. borealis draws with the console's own shared fonts
 * through plGetSharedFontByType, with a fallback chain per glyph, which is exactly
 * what docs/fonts.md said this platform was waiting for. The second is icons: a
 * person recognises a game by its picture long before they read its name, and there
 * is no way to show one on a text console.
 *
 * What has not changed is the rule the whole project rests on. Nothing in here
 * decides anything about save data. Every sentence on screen is still a
 * daemoon_str_id_t resolved through the tables, every destructive action still goes
 * through ui->confirm, and core is still the only thing that knows what a sync is.
 */
#pragma once

#include <borealis.hpp>

#include "daemoon_nx.h"

#include <daemoon/archive.h>
#include <daemoon/i18n.h>
#include <daemoon/sync.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace dm
{

/* ------------------------------------------------------------------- strings */

/* daemoon_str and daemoon_strf, as std::string, because that is what borealis
 * takes. No overload accepts a literal: the point of the string ids is that there
 * is no way to put a sentence on screen without going through the tables. */
std::string str(daemoon_str_id_t id);
std::string strf(const daemoon_str_ref_t* ref);
std::string strf(daemoon_str_id_t id, const std::vector<std::string>& args);

/* --------------------------------------------------------------------- theme */

/* borealis ships the Horizon palette. This replaces it with the one the web panel,
 * the README cover and the 3DS build already use, so that the console and the
 * browser look like the same piece of software rather than two.
 *
 * Called before the window is created, because the theme is read as views are
 * built. */
void applyTheme();

/* ---------------------------------------------------------------------- state */

/* One of everything, assembled once in main and handed to core.
 *
 * A singleton rather than a parameter threaded through every view: there is exactly
 * one console, one account and one server here, and the alternative was passing the
 * same pointer through four constructors to reach a button handler. */
struct App
{
    daemoon_env_t          env {};
    daemoon_archive_ctx_t  archive {};
    daemoon_nx_save_ctx_t  save {};
    daemoon_nx_config_t    config {};
    daemoon_net_curl_ctx_t net {};
    daemoon_nx_ui_ctx_t    ui {};

    daemoon_title_t* titles = nullptr;
    size_t           count  = 0;

    /* Core never sizes a buffer to a save; this is what it streams through. */
    unsigned char* scratch     = nullptr;
    size_t         scratchLen  = 0;

    /* Frees the previous list first, which is the backend's job rather than
     * free(). Returns what list_titles returned so the caller can say so. */
    daemoon_result_t reloadTitles();
    bool             canSync() const;
};

App& app();

/* ---------------------------------------------------------------- operations */

/* A backup, a sync or the conformance suite, run on a worker thread.
 *
 * On a thread because core's UI backend is blocking by design - confirm() returns
 * the answer, choose() returns the index - and borealis is a single main loop that
 * cannot be re-entered to ask a question. So the operation waits on the worker
 * while the dialog lives on the main thread, which is also what keeps the interface
 * drawing while a save is being packed.
 *
 * One at a time. Not a queue: two of these at once would be two writers to the same
 * save archive, and the mount only allows one anyway.
 */
bool operationRunning();
void runBackup(size_t index);
void runSync(size_t index);
void runSelfTest(size_t index);

/* Set by the operation runner so the UI backend below knows which thread it is on.
 * A confirmation asked from the main thread would deadlock waiting for the main
 * thread to draw it, and that is a bug worth failing loudly on rather than hanging.
 */
bool onWorkerThread();

/* Called by an operation when it finishes, on the main thread, so the list can be
 * re-read: a sync can change what is on the card, and the self test can empty a
 * save archive entirely. */
void setTitlesChangedCallback(std::function<void()> cb);

/* ------------------------------------------------------- the modal, and asking */

/* The screen an operation runs behind. Opened from the worker and drawn by the main
 * thread, which is the whole reason the operation is on a thread at all: a save
 * being packed used to be a frozen console with no way to say how far along it was.
 *
 * A dialog rather than an overlay because borealis dialogs are activities, so a
 * question asked in the middle of a sync - which version do you want to keep - simply
 * stacks on top of this one and pops back to it. */
void progressOpen(daemoon_str_id_t title);
void progressUpdate(const std::string& text, int pct);
void progressClose();

/* A question, from the worker thread, blocking it until the answer comes back.
 *
 * `cancelResult` is what B means. For a confirmation it is the same as "no", which
 * is also where the cursor starts; for a choice between two saves it is a negative,
 * because leaning toward either version is not this application's to do. Pass
 * ASK_NO_CANCEL when the question has to be answered. */
static constexpr int ASK_NO_CANCEL = -1000;
int askBlocking(const std::string& body, const std::vector<std::string>& options,
                int cancelResult);

/* ---------------------------------------------------------------------- views */

/* One save: its icon, its name, and the focus ring borealis draws around whichever
 * one the cursor is on. */
class MainScreen;

class SaveTile : public brls::Box
{
  public:
    SaveTile(MainScreen* screen, size_t index);

    void onFocusGained() override;

    size_t getIndex() const { return index; }

    /* The icon arrives later than the tile does - it is read off the ns service on
     * a thread, because sixty of those reads is a visible pause at startup. */
    void setIcon(const unsigned char* jpeg, size_t len);

  private:
    /* The screen is held rather than found by walking up the view tree. Three
     * getParent() calls happened to reach it and would have kept doing so until
     * somebody put the grid inside one more box. */
    MainScreen*  screen;
    size_t       index;
    brls::Image* icon  = nullptr;
    brls::Box*   blank = nullptr;
    brls::Label* name  = nullptr;
};

/* The whole screen: the grid on the left, what is known about the selected save on
 * the right, and the actions along the bottom. */
class MainScreen : public brls::Box
{
  public:
    MainScreen();

    void rebuild();
    void onTileFocused(size_t index);

  private:
    void buildGrid();
    void showDetails(size_t index);
    void loadIconsInBackground();

    brls::Box*   grid     = nullptr;
    brls::Box*   details  = nullptr;
    brls::Label* detName  = nullptr;
    brls::Label* detId    = nullptr;
    brls::Label* detNote  = nullptr;
    brls::Label* emptyMsg = nullptr;

    std::vector<SaveTile*> tiles;
    size_t                 selected = 0;
    /* Bumped on every rebuild. An icon that arrives after the list was re-read
     * belongs to a tile that no longer exists, and the generation is how that is
     * noticed instead of writing into freed memory. Atomic because the thread that
     * reads it is not the thread that writes it. */
    std::atomic<unsigned> generation { 0 };
    /* The first build happens inside the constructor, before this view is in a tree
     * and before there is anything to give focus to. */
    bool attached = false;
};

class MainActivity : public brls::Activity
{
  public:
    brls::View* createContentView() override;

  private:
    MainScreen* screen = nullptr;
};

} // namespace dm
