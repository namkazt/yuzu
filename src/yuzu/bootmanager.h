// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include <QImage>
#include <QThread>
#include <QWidget>

#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"

class QKeyEvent;
class QScreen;
class QTouchEvent;
class QStringList;
class QSurface;
class QOpenGLContext;
#ifdef HAS_VULKAN
class QVulkanInstance;
#endif

class GWidgetInternal;
class GGLWidgetInternal;
class GVKWidgetInternal;
class GMainWindow;
class GRenderWindow;
class QSurface;
class QOpenGLContext;

namespace VideoCore {
enum class LoadCallbackStage;
}

class EmuThread final : public QThread {
    Q_OBJECT

public:
    explicit EmuThread(GRenderWindow* render_window);
    ~EmuThread() override;

    /**
     * Start emulation (on new thread)
     * @warning Only call when not running!
     */
    void run() override;

    /**
     * Steps the emulation thread by a single CPU instruction (if the CPU is not already running)
     * @note This function is thread-safe
     */
    void ExecStep() {
        exec_step = true;
        running_cv.notify_all();
    }

    /**
     * Sets whether the emulation thread is running or not
     * @param running Boolean value, set the emulation thread to running if true
     * @note This function is thread-safe
     */
    void SetRunning(bool running) {
        std::unique_lock lock{running_mutex};
        this->running = running;
        lock.unlock();
        running_cv.notify_all();
    }

    /**
     * Check if the emulation thread is running or not
     * @return True if the emulation thread is running, otherwise false
     * @note This function is thread-safe
     */
    bool IsRunning() const {
        return running;
    }

    /**
     * Requests for the emulation thread to stop running
     */
    void RequestStop() {
        stop_run = true;
        SetRunning(false);
    }

private:
    bool exec_step = false;
    bool running = false;
    std::atomic_bool stop_run{false};
    std::mutex running_mutex;
    std::condition_variable running_cv;

    GRenderWindow* render_window;

signals:
    /**
     * Emitted when the CPU has halted execution
     *
     * @warning When connecting to this signal from other threads, make sure to specify either
     * Qt::QueuedConnection (invoke slot within the destination object's message thread) or even
     * Qt::BlockingQueuedConnection (additionally block source thread until slot returns)
     */
    void DebugModeEntered();

    /**
     * Emitted right before the CPU continues execution
     *
     * @warning When connecting to this signal from other threads, make sure to specify either
     * Qt::QueuedConnection (invoke slot within the destination object's message thread) or even
     * Qt::BlockingQueuedConnection (additionally block source thread until slot returns)
     */
    void DebugModeLeft();

    void ErrorThrown(Core::System::ResultStatus, std::string);

    void LoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);
};

class GRenderWindow : public QWidget, public Core::Frontend::EmuWindow {
    Q_OBJECT

public:
    GRenderWindow(GMainWindow* parent, EmuThread* emu_thread);
    ~GRenderWindow() override;

    // EmuWindow implementation
    void SwapBuffers() override;
    void MakeCurrent() override;
    void DoneCurrent() override;
    void PollEvents() override;
    bool IsShown() const override;
    void RetrieveVulkanHandlers(void* get_instance_proc_addr, void* instance,
                                void* surface) const override;
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;

    void ForwardKeyPressEvent(QKeyEvent* event);
    void ForwardKeyReleaseEvent(QKeyEvent* event);

    void BackupGeometry();
    void RestoreGeometry();
    void restoreGeometry(const QByteArray& geometry); // overridden
    QByteArray saveGeometry();                        // overridden

    qreal GetWindowPixelRatio() const;
    std::pair<u32, u32> ScaleTouch(QPointF pos) const;

    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

    void OnClientAreaResized(u32 width, u32 height);

    bool InitRenderTarget();

    void CaptureScreenshot(u32 res_scale, const QString& screenshot_path);

public slots:
    void moveContext(); // overridden

    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();
    void OnFramebufferSizeChanged();

signals:
    /// Emitted when the window is closed
    void Closed();
    void FirstFrameDisplayed();

private:
    void TouchBeginEvent(const QTouchEvent* event);
    void TouchUpdateEvent(const QTouchEvent* event);
    void TouchEndEvent();

    void OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) override;

    bool InitializeOpenGL();
    bool InitializeVulkan();
    bool LoadOpenGL();
    QStringList GetUnsupportedGLExtensions() const;

    QWidget* container = nullptr;
    GWidgetInternal* child = nullptr;

    EmuThread* emu_thread;
    // Context that backs the GGLWidgetInternal (and will be used by core to render)
    std::unique_ptr<QOpenGLContext> context;
    // Context that will be shared between all newly created contexts. This should never be made
    // current
    std::unique_ptr<QOpenGLContext> shared_context;

#ifdef HAS_VULKAN
    std::unique_ptr<QVulkanInstance> vk_instance;
#endif

    /// Temporary storage of the screenshot taken
    QImage screenshot_image;

    QByteArray geometry;
    bool first_frame = false;

protected:
    void showEvent(QShowEvent* event) override;
};
