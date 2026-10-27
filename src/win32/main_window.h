#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commctrl.h>

#include "task_list_model.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace litefetch {
class TaskWorker;
}

namespace litefetch::ui {

struct AppOptions {
    std::filesystem::path databasePath;
    std::filesystem::path downloadDirectory;
};

// Native main window: OS-drawn caption, native menu, compact toolbar, Details
// ListView and the four-part status bar. It only shows what the worker really
// reports; commands that are not implemented stay disabled with a tooltip that
// states the reason.
class MainWindow {
public:
    explicit MainWindow(AppOptions options);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(HINSTANCE instance, int showCommand);
    HWND handle() const { return hwnd_; }
    HACCEL accelerators() const { return accelerators_; }

private:
    static LRESULT CALLBACK windowProcStatic(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool createControls();
    void createFont();
    void createToolbar();
    void createList();
    void createStatusBar();
    void createTooltip();
    void applyFont();
    void layoutControls();
    void updateLayoutMetrics();
    void applyColumnWidths();
    void refreshFromWorker();
    void syncList();
    void updateStatus();
    void updateActions();
    void updateTooltip(NMTTDISPINFOW& info);
    void selectRow(std::size_t index);
    void ensureSelection();
    void startSelected();
    void stopSelected();
    void openFolderSelected();
    void copyLinkSelected();
    void openNewDownload();
    void showCapabilities();
    void showAbout();
    void beginClose();
    void finishClose();
    void setShutdownStatus();
    int selectedIndex() const;
    const TaskRow* selectedRow() const;
    bool isClosing() const { return closing_; }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND toolbar_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND tooltip_ = nullptr;
    HMENU menu_ = nullptr;
    HACCEL accelerators_ = nullptr;
    HFONT font_ = nullptr;
    HICON largeIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    AppOptions options_;
    std::unique_ptr<TaskWorker> worker_;
    TaskListModel model_;
    std::vector<std::string> displayedIds_;
    std::string selectedId_;
    std::string lastErrorText_;
    std::uint64_t lastErrorSequence_ = 0;
    std::chrono::steady_clock::time_point lastErrorAt_{};
    std::wstring tooltipText_;
    HIMAGELIST toolbarImages_ = nullptr;
    HIMAGELIST rowImages_ = nullptr;
    int lastColumnClientWidth_ = -1;
    int lastColumnDpi_ = 0;
    int startImageDisabled_ = -1;
    int startImageEnabled_ = -1;
    int toolbarHeight_ = 0;
    int dpi_ = 96;
    bool closing_ = false;
    bool closingWorkerReleased_ = false;
};

} // namespace litefetch::ui
