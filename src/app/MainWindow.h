#pragma once

#include "core/common/Common.h"
#include "core/pipeline/StreamPipeline.h"

#include <QElapsedTimer>
#include <QMainWindow>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

class QComboBox;
class QFormLayout;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();

    void refreshVideoDevices();
    void refreshAudioDevices();
    void updateSourceFields();

    void startStreaming();
    void stopStreaming();
    void updateUiState();

    void pollStats();
    void appendLog(const QString& message);
    void postLog(const std::string& message);

    void onPreviewFrame(const VideoFramePtr& frame);

    StreamConfig collectConfig() const;

    QWidget* previewWidget_ = nullptr;
    std::chrono::steady_clock::time_point lastPreviewTime_;

    QFormLayout* videoForm_ = nullptr;
    QHBoxLayout* cameraRow_ = nullptr;
    QComboBox* videoSourceCombo_ = nullptr;
    QComboBox* cameraCombo_ = nullptr;
    QPushButton* refreshCameraButton_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QSpinBox* bitrateSpin_ = nullptr;

    QComboBox* audioSourceCombo_ = nullptr;
    QComboBox* audioDeviceCombo_ = nullptr;
    QPushButton* refreshAudioButton_ = nullptr;

    QPlainTextEdit* targetsEdit_ = nullptr;

    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* stateLabel_ = nullptr;

    QLabel* durationLabel_ = nullptr;
    QLabel* videoLabel_ = nullptr;
    QLabel* audioLabel_ = nullptr;
    QLabel* targetLabel_ = nullptr;

    QPlainTextEdit* logView_ = nullptr;
    QTimer* statsTimer_ = nullptr;

    StreamPipeline pipeline_;

    std::mutex pipelineMutex_;

    std::atomic<bool> starting_{false};
    std::atomic<bool> running_{false};

    std::thread worker_;

    QElapsedTimer elapsed_;
};
