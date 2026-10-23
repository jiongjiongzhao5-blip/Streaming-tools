#include "MainWindow.h"

#include "PreviewWidget.h"

#include "core/capture/AudioCapturer.h"
#include "core/capture/CameraCapturer.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSize>
#include <QSpinBox>
#include <QStringList>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    buildUi();

    refreshVideoDevices();
    refreshAudioDevices();
    updateSourceFields();
    updateUiState();

    appendLog("就绪。填好推流地址后点「开始推流」。");
}

MainWindow::~MainWindow()
{
    if (worker_.joinable())
        worker_.join();
}

void MainWindow::buildUi()
{
    setWindowTitle("LightLiveStreamer");
    resize(1180, 800);

    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);

    auto* videoGroup = new QGroupBox("视频源", central);
    videoForm_ = new QFormLayout(videoGroup);

    videoSourceCombo_ = new QComboBox(videoGroup);
    videoSourceCombo_->addItem("屏幕");
    videoSourceCombo_->addItem("摄像头");
    videoForm_->addRow("来源", videoSourceCombo_);

    cameraCombo_ = new QComboBox(videoGroup);
    refreshCameraButton_ = new QPushButton("刷新", videoGroup);
    refreshCameraButton_->setFixedWidth(64);
    cameraRow_ = new QHBoxLayout;
    cameraRow_->addWidget(cameraCombo_, 1);
    cameraRow_->addWidget(refreshCameraButton_);
    videoForm_->addRow("设备", cameraRow_);

    resolutionCombo_ = new QComboBox(videoGroup);
    resolutionCombo_->addItem("640 x 360", QSize(640, 360));
    resolutionCombo_->addItem("1280 x 720", QSize(1280, 720));
    resolutionCombo_->addItem("1920 x 1080", QSize(1920, 1080));
    resolutionCombo_->setCurrentIndex(1);
    videoForm_->addRow("分辨率", resolutionCombo_);

    fpsSpin_ = new QSpinBox(videoGroup);
    fpsSpin_->setRange(5, 60);
    fpsSpin_->setValue(30);
    fpsSpin_->setSuffix(" fps");
    videoForm_->addRow("帧率", fpsSpin_);

    bitrateSpin_ = new QSpinBox(videoGroup);
    bitrateSpin_->setRange(300, 20000);
    bitrateSpin_->setSingleStep(500);
    bitrateSpin_->setValue(2500);
    bitrateSpin_->setSuffix(" kbps");
    videoForm_->addRow("码率", bitrateSpin_);

    auto* audioGroup = new QGroupBox("音频源", central);
    auto* audioForm = new QFormLayout(audioGroup);

    audioSourceCombo_ = new QComboBox(audioGroup);
    audioSourceCombo_->addItem("系统声音");
    audioSourceCombo_->addItem("麦克风");
    audioForm->addRow("来源", audioSourceCombo_);

    audioDeviceCombo_ = new QComboBox(audioGroup);
    refreshAudioButton_ = new QPushButton("刷新", audioGroup);
    refreshAudioButton_->setFixedWidth(64);
    auto* audioRow = new QHBoxLayout;
    audioRow->addWidget(audioDeviceCombo_, 1);
    audioRow->addWidget(refreshAudioButton_);
    audioForm->addRow("设备", audioRow);

    previewWidget_ = new PreviewWidget(central);

    auto* configPanel = new QWidget(central);
    auto* configLayout = new QVBoxLayout(configPanel);
    configLayout->setContentsMargins(0, 0, 0, 0);
    configLayout->addWidget(videoGroup);
    configLayout->addWidget(audioGroup);

    auto* targetGroup = new QGroupBox("推流地址", central);
    auto* targetLayout = new QVBoxLayout(targetGroup);

    targetsEdit_ = new QPlainTextEdit(targetGroup);
    targetsEdit_->setMaximumHeight(88);
    targetsEdit_->setPlaceholderText(
        "每行一个地址，可以写成「名称=地址」方便区分：\n"
        "bilibili=rtmp://live-push.bilivideo.com/live-bvc/?streamname=...\n"
        "douyin=rtmp://push-rtmp-l6.douyincdn.com/stage/...");
    targetLayout->addWidget(targetsEdit_);

    auto* controlLayout = new QHBoxLayout;

    startButton_ = new QPushButton("开始推流", central);
    stopButton_ = new QPushButton("停止推流", central);
    stopButton_->setEnabled(false);
    stateLabel_ = new QLabel("未开始", central);

    QFont bold = stateLabel_->font();
    bold.setBold(true);
    stateLabel_->setFont(bold);

    controlLayout->addWidget(startButton_);
    controlLayout->addWidget(stopButton_);
    controlLayout->addSpacing(16);
    controlLayout->addWidget(stateLabel_);
    controlLayout->addStretch();

    auto* statsLayout = new QHBoxLayout;

    durationLabel_ = new QLabel("时长 00:00:00", central);
    videoLabel_ = new QLabel("视频 -", central);
    audioLabel_ = new QLabel("音频 -", central);
    targetLabel_ = new QLabel("连接 -", central);

    statsLayout->addWidget(durationLabel_);
    statsLayout->addSpacing(16);
    statsLayout->addWidget(videoLabel_);
    statsLayout->addSpacing(16);
    statsLayout->addWidget(audioLabel_);
    statsLayout->addSpacing(16);
    statsLayout->addWidget(targetLabel_);
    statsLayout->addStretch();

    auto* logGroup = new QGroupBox("日志", central);
    auto* logLayout = new QVBoxLayout(logGroup);

    logView_ = new QPlainTextEdit(logGroup);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    logLayout->addWidget(logView_);

    configLayout->addWidget(targetGroup);
    configLayout->addLayout(controlLayout);
    configLayout->addLayout(statsLayout);
    configLayout->addStretch();

    auto* topLayout = new QHBoxLayout;
    topLayout->addWidget(previewWidget_, 3);
    topLayout->addWidget(configPanel, 2);

    mainLayout->addLayout(topLayout, 3);
    mainLayout->addWidget(logGroup, 1);

    setCentralWidget(central);

    connect(videoSourceCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateSourceFields(); });
    connect(audioSourceCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshAudioDevices(); });
    connect(refreshCameraButton_, &QPushButton::clicked, this,
            &MainWindow::refreshVideoDevices);
    connect(refreshAudioButton_, &QPushButton::clicked, this,
            &MainWindow::refreshAudioDevices);
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startStreaming);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopStreaming);

    statsTimer_ = new QTimer(this);
    statsTimer_->setInterval(500);
    connect(statsTimer_, &QTimer::timeout, this, &MainWindow::pollStats);
}

void MainWindow::refreshVideoDevices()
{
    const QString previous = cameraCombo_->currentText();

    cameraCombo_->clear();

    const std::vector<std::string> devices = CameraCapturer::listDevices();
    for (const std::string& name : devices)
        cameraCombo_->addItem(QString::fromStdString(name));

    if (devices.empty()) {
        cameraCombo_->addItem("（没有找到摄像头）");
        appendLog("没有枚举到任何摄像头");
    } else {
        const int index = cameraCombo_->findText(previous);
        if (index >= 0)
            cameraCombo_->setCurrentIndex(index);
        appendLog(QString("枚举到 %1 个摄像头").arg(devices.size()));
    }
}

void MainWindow::refreshAudioDevices()
{
    const AudioSourceType type = audioSourceCombo_->currentIndex() == 0
                                     ? AudioSourceType::SystemAudio
                                     : AudioSourceType::Microphone;

    audioDeviceCombo_->clear();
    audioDeviceCombo_->addItem("（系统默认）", -1);

    const std::vector<std::string> devices = AudioCapturer::listDevices(type);
    for (size_t i = 0; i < devices.size(); ++i) {
        audioDeviceCombo_->addItem(QString::fromStdString(devices[i]),
                                   static_cast<int>(i));
    }
}

void MainWindow::updateSourceFields()
{
    const bool isCamera = videoSourceCombo_->currentIndex() == 1;

    if (videoForm_ && cameraRow_)
        videoForm_->setRowVisible(cameraRow_, isCamera);
}

StreamConfig MainWindow::collectConfig() const
{
    StreamConfig config;

    config.sourceType = videoSourceCombo_->currentIndex() == 1 ? SourceType::Camera
                                                               : SourceType::Screen;
    if (config.sourceType == SourceType::Camera)
        config.cameraIndex = std::max(0, cameraCombo_->currentIndex());

    const QSize size = resolutionCombo_->currentData().toSize();
    config.width = size.width();
    config.height = size.height();
    config.fps = fpsSpin_->value();
    config.videoKbps = bitrateSpin_->value();

    config.audioType = audioSourceCombo_->currentIndex() == 0
                           ? AudioSourceType::SystemAudio
                           : AudioSourceType::Microphone;
    config.audioDeviceIndex = audioDeviceCombo_->currentData().toInt();

    config.sampleRate = 48000;
    config.channels = 2;
    config.audioKbps = 128;

    const QStringList lines = targetsEdit_->toPlainText().split('\n');
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        StreamTarget target;
        const int separator = line.indexOf('=');
        if (separator > 0) {
            target.platform = line.left(separator).trimmed().toStdString();
            target.url = line.mid(separator + 1).trimmed().toStdString();
        } else {
            target.url = line.toStdString();
        }

        if (target.platform.empty())
            target.platform = "路" + std::to_string(config.targets.size() + 1);

        if (target.url.empty())
            continue;

        config.targets.push_back(target);
    }

    return config;
}

void MainWindow::startStreaming()
{
    if (starting_.load() || running_.load())
        return;

    const StreamConfig config = collectConfig();
    if (config.targets.empty()) {
        appendLog("还没有填推流地址");
        return;
    }

    if (worker_.joinable())
        worker_.join();

    starting_ = true;
    updateUiState();
    appendLog(QString("正在连接 %1 路...").arg(config.targets.size()));

    worker_ = std::thread([this, config] {
        bool ok = false;
        QString error;

        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);

            pipeline_.setPreviewCallback(
                [this](const VideoFramePtr& frame) { onPreviewFrame(frame); });

            ok = pipeline_.start(config, [this](const std::string& message) {
                postLog(message);
            });
            if (!ok)
                error = QString::fromStdString(pipeline_.lastError());
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, error] {
                starting_ = false;
                running_ = ok;

                if (ok) {
                    elapsed_.start();
                    statsTimer_->start();
                    appendLog("推流已开始");
                } else {
                    appendLog("启动失败: " + error);
                }
                updateUiState();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::stopStreaming()
{
    statsTimer_->stop();

    if (!running_.exchange(false)) {
        updateUiState();
        return;
    }

    if (worker_.joinable())
        worker_.join();

    updateUiState();
    appendLog("正在停止...");

    worker_ = std::thread([this] {
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            pipeline_.stop();
            pipeline_.setPreviewCallback(nullptr);
        }
        QMetaObject::invokeMethod(
            this,
            [this] {
                appendLog("已停止");
                if (previewWidget_)
                    static_cast<PreviewWidget*>(previewWidget_)->clear();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::updateUiState()
{
    const bool starting = starting_.load();
    const bool running = running_.load();

    startButton_->setEnabled(!starting && !running);
    stopButton_->setEnabled(running && !starting);

    if (starting)
        stateLabel_->setText("正在连接...");
    else if (running)
        stateLabel_->setText("推流中");
    else
        stateLabel_->setText("未开始");

    const bool editable = !starting && !running;

    videoSourceCombo_->setEnabled(editable);
    cameraCombo_->setEnabled(editable);
    refreshCameraButton_->setEnabled(editable);
    resolutionCombo_->setEnabled(editable);
    fpsSpin_->setEnabled(editable);
    bitrateSpin_->setEnabled(editable);

    audioSourceCombo_->setEnabled(editable);
    audioDeviceCombo_->setEnabled(editable);
    refreshAudioButton_->setEnabled(editable);

    targetsEdit_->setEnabled(editable);
}

void MainWindow::pollStats()
{
    PipelineStats stats;
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        stats = pipeline_.stats();
    }

    const qint64 totalSeconds = elapsed_.elapsed() / 1000;
    durationLabel_->setText(
        QString("时长 %1:%2:%3")
            .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
            .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(totalSeconds % 60, 2, 10, QLatin1Char('0')));

    videoLabel_->setText(QString("视频 %1 帧 / %2 包 / 丢 %3")
                             .arg(stats.videoFramesCaptured)
                             .arg(stats.videoPacketsEncoded)
                             .arg(stats.videoFramesDropped));

    audioLabel_->setText(
        QString("音频 %1 块 / %2 包").arg(stats.audioChunksCaptured).arg(
            stats.audioPacketsEncoded));

    targetLabel_->setText(
        QString("连接 %1/%2").arg(stats.connectedTargets).arg(stats.totalTargets));
}

void MainWindow::appendLog(const QString& message)
{
    logView_->appendPlainText(
        QString("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss")).arg(message));
}

void MainWindow::postLog(const std::string& message)
{
    const QString text = QString::fromStdString(message);
    QMetaObject::invokeMethod(this, [this, text] { appendLog(text); },
                              Qt::QueuedConnection);
}

void MainWindow::onPreviewFrame(const VideoFramePtr& frame)
{
    if (!frame || frame->bgra.empty() || frame->width <= 0 || frame->height <= 0)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastPreviewTime_ < std::chrono::milliseconds(66))
        return;
    lastPreviewTime_ = now;

    auto* holder = new VideoFramePtr(frame);
    QImage image(frame->bgra.data(), frame->width, frame->height, frame->width * 4,
                 QImage::Format_ARGB32,
                 [](void* info) { delete static_cast<VideoFramePtr*>(info); },
                 holder);

    QMetaObject::invokeMethod(
        this,
        [this, image] {
            if (previewWidget_)
                static_cast<PreviewWidget*>(previewWidget_)->setImage(image);
        },
        Qt::QueuedConnection);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    statsTimer_->stop();

    if (running_.exchange(false) || starting_.exchange(false)) {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        pipeline_.stop();
    }

    if (worker_.joinable())
        worker_.join();

    QMainWindow::closeEvent(event);
}
