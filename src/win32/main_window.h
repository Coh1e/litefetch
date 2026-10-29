#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include "batch_dialog.h"
#include "bt_engine.h"
#include "batch_input.h"
#include "columns.h"
#include "startup.h"
#include "task_list_model.h"
#include "task_view.h"
#include "task_worker.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace litefetch::ui {

// Native main window: OS-drawn caption, native menu, compact toolbar, Details
// ListView, temporary find overlay and the four-part status bar. Commands are
// dispatched to the single TaskWorker boundary; the window only displays
// persisted states and never fabricates transfer progress.
class MainWindow {
public:
    explicit MainWindow(AppOptions options);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(HINSTANCE instance, int showCommand);
    HWND handle() const { return hwnd_; }
    // Exposed for the window harness: whether the notification icon is up.
    bool trayIconVisible() const { return trayVisible_; }
    HACCEL accelerators() const { return accelerators_; }
    // Modeless owned dialogs (delete confirmation) are pumped by the main
    // loop so IsDialogMessage can provide native Tab/Esc/Enter behavior.
    HWND activeDialog() const { return deleteDialog_; }

private:
    struct PendingLine {
        BatchLine line;
        bool submitted = false;
        bool done = false;
        bool ok = false;
        bool existing = false;
        std::string taskId;
        std::string destination;
        std::string error;
    };
    struct PendingBatch {
        std::uint64_t id = 0;
        bool showDialog = false;
        std::vector<PendingLine> lines;
        std::size_t submittedCount = 0;
        std::size_t completedCount = 0;
    };

    static LRESULT CALLBACK windowProcStatic(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK searchSubclassStatic(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR reference);
    static INT_PTR CALLBACK deleteDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool createControls();
    void addTrayIcon();
    void removeTrayIcon();
    void showTrayMenu();

    void createFont();
    void createToolbar();
    void createList();
    void createStatusBar();
    void createTooltip();
    void createSearchOverlay();
    void applyFont();
    void layoutControls();
    void layoutStatusParts();
    void updateLayoutMetrics();
    void applyColumnWidths();
    void rebuildColumns();
    void applySortArrow();
    void rebuildView();
    void syncList();
    void updateStatus();
    void updateActions();
    void updateTooltip(NMTTDISPINFOW& info);
    void refreshFromWorker();
    // A window procedure must not let a C++ exception cross the OS boundary:
    // that aborts the process without an exit code. Failures are written next
    // to the task database so an acceptance run keeps the real reason.
    void logUiFailure(std::string_view message) const;

    void handleAddResults(const WorkerSnapshot& snapshot);
    void handleOutcomes(const WorkerSnapshot& snapshot);
    void finalizeBatch(PendingBatch& batch);
    void selectTaskById(const std::string& taskId, bool clearFilters);
    void setNotice(std::string text);

    void showSearch();
    void hideSearch();
    void updateSearchQuery();
    void applyFilter(TaskFilter filter);
    void showColumnChooser();
    void toggleToolbar();
    void toggleStatusBar();

    void promptForCredentials(const std::string& taskId);
    void showProperties();
    void openNewDownload();
    void openBatchAdd(std::string initialText = {});
    void submitBatch(const BatchAddRequest& request, bool showDialog);
    void pasteFromClipboard();
    void handleDrop(HDROP drop);
    void openTorrentBoundary();
    void beginTorrentAdd(const std::string& source);
    void handleTorrentMetadata(BtMetadataResult metadata, const std::string& source);

    void startSelected();
    void verifySelected();
    void pauseSelected();
    void stopSelected();
    void deleteSelected();
    void finishDeleteDialog(bool confirmed);
    void openFileSelected();
    void openFolderSelected();
    void copySelectedLinks();
    void copySelectedPaths();
    void copyText(const std::wstring& text, std::string notice);
    void selectAllVisible();

    void showCapabilities();
    void showAbout();
    void beginClose();
    void finishClose();
    void setShutdownStatus();

    std::vector<std::string> selectedIds() const;
    std::string focusedId() const;
    int viewIndexOf(const std::string& taskId) const;
    const TaskRow* rowForId(const std::string& taskId) const;
    bool isClosing() const { return closing_; }
    std::filesystem::path uiStatePath() const;
    void saveColumnSettings();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND toolbar_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND searchClose_ = nullptr;
    HWND deleteDialog_ = nullptr;
    std::vector<std::string> pendingDeleteSelection_;
    HMENU menu_ = nullptr;
    HACCEL accelerators_ = nullptr;
    HFONT font_ = nullptr;
    bool trayVisible_ = false;
    HICON largeIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    AppOptions options_;
    std::unique_ptr<TaskWorker> worker_;
    TaskListModel model_;
    ColumnSettings columnSettings_;
    std::vector<ColumnId> visibleColumns_;
    std::vector<TaskRow> viewRows_;
    std::vector<std::string> displayedIds_;
    TaskFilter filter_ = TaskFilter::All;
    std::string query_;
    std::string pendingSelectTaskId_;
    std::string lastErrorText_;
    std::uint64_t lastErrorSequence_ = 0;
    std::chrono::steady_clock::time_point lastErrorAt_{};
    std::wstring noticeText_;
    std::chrono::steady_clock::time_point noticeAt_{};
    std::map<std::uint64_t, PendingBatch> pendingBatches_;
    std::uint64_t nextBatchId_ = 1;
    std::uint64_t lastAddSequence_ = 0;
    std::uint64_t lastOutcomeSequence_ = 0;
    std::set<std::string, std::less<>> pendingDeleteIds_;
    std::set<std::string, std::less<>> promptedCredentials_;
    std::shared_ptr<std::atomic_bool> torrentFetchCancel_;
    bool torrentFetchActive_ = false;
    std::vector<std::string> pendingDeleteFailures_;
    std::size_t pendingDeleteTotal_ = 0;
    std::wstring tooltipText_;
    HIMAGELIST toolbarImages_ = nullptr;
    HIMAGELIST rowImages_ = nullptr;
    int lastColumnClientWidth_ = -1;
    int lastColumnDpi_ = 0;
    int startImageDisabled_ = -1;
    int startImageEnabled_ = -1;
    int toolbarHeight_ = 0;
    int dpi_ = 96;
    bool showToolbar_ = true;
    bool showStatusBar_ = true;
    bool searchVisible_ = false;
    bool columnsDirty_ = true;
    bool initialSelectionDone_ = false;
    bool suppressSelection_ = false;
    bool closing_ = false;
    bool closingWorkerReleased_ = false;
    std::wstring statusTips_[4];
};

} // namespace litefetch::ui
