#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QRegularExpression>
#include <QDateTime>
#include <QApplication>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_isStreaming(false)
{
    setupUi();
    
    m_streamProcess = new QProcess(this);
    connect(m_streamProcess, &QProcess::readyReadStandardError, this, &MainWindow::onProcessReadyReadStandardError);
    // Connect standard output just in case
    connect(m_streamProcess, &QProcess::readyReadStandardOutput, [this](){
        QString out = QString::fromUtf8(m_streamProcess->readAllStandardOutput());
        if (!out.trimmed().isEmpty()) {
            logMessage("FFmpeg stdout: " + out.trimmed());
        }
    });
    connect(m_streamProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::onProcessFinished);

    discoverDevices();
}

MainWindow::~MainWindow()
{
    stopStreaming();
}

void MainWindow::setupUi()
{
    setWindowTitle("OBS SRT Meeting Client");
    resize(640, 480);

    // Apply Premium QSS Dark Stylesheet
    QString styleSheet = R"(
        QMainWindow {
            background-color: #1e1e2e;
            color: #cdd6f4;
            font-family: 'Segoe UI', 'Roboto', 'Helvetica Neue', sans-serif;
        }
        QLabel {
            color: #bac2de;
            font-size: 13px;
            font-weight: 500;
        }
        QLineEdit, QSpinBox, QComboBox {
            background-color: #313244;
            color: #cdd6f4;
            border: 1px solid #45475a;
            border-radius: 6px;
            padding: 6px 12px;
            font-size: 13px;
        }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
            border: 1px solid #cba6f7;
            background-color: #45475a;
        }
        QComboBox::drop-down {
            border: none;
            width: 20px;
        }
        QPushButton {
            background-color: #b4befe;
            color: #11111b;
            border: none;
            border-radius: 6px;
            padding: 8px 16px;
            font-size: 13px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #cba6f7;
        }
        QPushButton:pressed {
            background-color: #a6e3a1;
        }
        QTextEdit {
            background-color: #181825;
            color: #a6e3a1;
            border: 1px solid #313244;
            border-radius: 8px;
            padding: 8px;
            font-family: 'Courier New', Courier, monospace;
            font-size: 12px;
        }
        QGroupBox {
            border: 1px solid #313244;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 12px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            padding: 0 8px;
            color: #cba6f7;
            font-weight: bold;
        }
    )";
    setStyleSheet(styleSheet);

    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    // Title Banner
    QLabel *titleLabel = new QLabel("OBS SRT Meeting Participant", this);
    titleLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #cba6f7;");
    mainLayout->addWidget(titleLabel);

    // Form Layout Container
    QWidget *formContainer = new QWidget(this);
    formContainer->setStyleSheet("background-color: #252538; border-radius: 8px;");
    QVBoxLayout *formContainerLayout = new QVBoxLayout(formContainer);
    formContainerLayout->setContentsMargins(12, 12, 12, 12);

    QFormLayout *formLayout = new QFormLayout();
    formLayout->setSpacing(10);
    formLayout->setLabelAlignment(Qt::AlignRight);

    // Host IP
    m_ipEdit = new QLineEdit("127.0.0.1", this);
    m_ipEdit->setPlaceholderText("e.g. 192.168.1.100");
    formLayout->addRow("Broadcaster IP:", m_ipEdit);

    // Port
    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(9000);
    formLayout->addRow("SRT Port:", m_portSpin);

    // Display Name
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("e.g. Jane_Doe");
    formLayout->addRow("Display Name:", m_nameEdit);

    // Video Capture Selection
    m_videoCombo = new QComboBox(this);
    formLayout->addRow("Camera:", m_videoCombo);

    // Audio Capture Selection
    m_audioCombo = new QComboBox(this);
    formLayout->addRow("Microphone:", m_audioCombo);

    formContainerLayout->addLayout(formLayout);
    mainLayout->addWidget(formContainer);

    // Action Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_refreshButton = new QPushButton("Refresh Devices", this);
    m_refreshButton->setStyleSheet("background-color: #45475a; color: #cdd6f4;");
    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    buttonLayout->addWidget(m_refreshButton);

    m_connectButton = new QPushButton("Connect & Stream", this);
    m_connectButton->setStyleSheet("background-color: #a6e3a1; color: #11111b; font-weight: bold;");
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    buttonLayout->addWidget(m_connectButton);

    mainLayout->addLayout(buttonLayout);

    // Logs Output
    QLabel *logLabel = new QLabel("Connection Logs & Diagnostics:", this);
    logLabel->setStyleSheet("font-weight: bold;");
    mainLayout->addWidget(logLabel);

    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    mainLayout->addWidget(m_logEdit);

    logMessage("Ready. Set broadcaster IP, port, name and choose devices.");
}

void MainWindow::discoverDevices()
{
    m_videoCombo->clear();
    m_audioCombo->clear();

    logMessage("Scanning for audio/video capture devices...");

#if defined(Q_OS_LINUX)
    // 1. Discover Video Devices via /sys/class/video4linux
    QDir v4lDir("/sys/class/video4linux");
    if (v4lDir.exists()) {
        QStringList videoDevices = v4lDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &dev : videoDevices) {
            QString namePath = QString("/sys/class/video4linux/%1/name").arg(dev);
            QFile nameFile(namePath);
            QString friendlyName = dev;
            if (nameFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                friendlyName = QString::fromUtf8(nameFile.readAll()).trimmed();
            }
            m_videoCombo->addItem(QString("%1 (/dev/%2)").arg(friendlyName, dev), QString("/dev/%1").arg(dev));
        }
    }
    if (m_videoCombo->count() == 0) {
        m_videoCombo->addItem("No cameras found (or manually type device)", "");
    }

    // 2. Discover Audio Devices (ALSA/Pulse)
    m_audioCombo->addItem("Default Audio (ALSA/Pulse)", "default");
    
    QProcess arecordProc;
    arecordProc.start("arecord", QStringList() << "-L");
    if (arecordProc.waitForFinished(1000)) {
        QString output = QString::fromUtf8(arecordProc.readAllStandardOutput());
        QStringList lines = output.split('\n');
        for (const QString &line : lines) {
            // Filter out continuation lines (start with space before trimming)
            // and exclude devices containing colons or "HW" which are hardware
            // identifiers rather than user-friendly device names
            if (line.startsWith(" ") || line.trimmed().isEmpty()) continue;
            QString trimmed = line.trimmed();
            if (!trimmed.startsWith("null") && !trimmed.contains(":") && !trimmed.contains("HW")) {
                m_audioCombo->addItem(trimmed, trimmed);
            }
        }
    }
#elif defined(Q_OS_WIN)
    // Run FFmpeg to list DirectShow devices on Windows
    QProcess ffmpegProc;
    ffmpegProc.start("ffmpeg", QStringList() << "-list_devices" << "true" << "-f" << "dshow" << "-i" << "dummy");
    if (ffmpegProc.waitForFinished(2000)) {
        QString errOutput = QString::fromUtf8(ffmpegProc.readAllStandardError());
        
        enum ParseState { FINDING_VIDEO, PARSING_VIDEO, FINDING_AUDIO, PARSING_AUDIO };
        ParseState state = FINDING_VIDEO;
        
        QStringList lines = errOutput.split('\n');
        for (const QString &line : lines) {
            if (line.contains("DirectShow video devices")) {
                state = PARSING_VIDEO;
                continue;
            } else if (line.contains("DirectShow audio devices")) {
                state = PARSING_AUDIO;
                continue;
            }
            
            QRegularExpression nameRegex("\"([^\"]+)\"");
            QRegularExpressionMatch match = nameRegex.match(line);
            if (match.hasMatch()) {
                QString devName = match.captured(1);
                if (state == PARSING_VIDEO) {
                    m_videoCombo->addItem(devName, devName);
                } else if (state == PARSING_AUDIO) {
                    m_audioCombo->addItem(devName, devName);
                }
            }
        }
    }
    
    if (m_videoCombo->count() == 0) {
        m_videoCombo->addItem("Integrated Camera", "Integrated Camera");
        m_videoCombo->addItem("OBS Virtual Camera", "OBS Virtual Camera");
    }
    if (m_audioCombo->count() == 0) {
        m_audioCombo->addItem("Default Microphone", "default");
    }
#elif defined(Q_OS_MAC)
    // Run FFmpeg to list AVFoundation devices on macOS
    QProcess ffmpegProc;
    ffmpegProc.start("ffmpeg", QStringList() << "-list_devices" << "true" << "-f" << "avfoundation" << "-i" << "");
    if (ffmpegProc.waitForFinished(2000)) {
        QString errOutput = QString::fromUtf8(ffmpegProc.readAllStandardError());
        bool inVideo = false;
        bool inAudio = false;
        QStringList lines = errOutput.split('\n');
        for (const QString &line : lines) {
            if (line.contains("AVFoundation video devices")) { inVideo = true; inAudio = false; continue; }
            if (line.contains("AVFoundation audio devices")) { inAudio = true; inVideo = false; continue; }
            // Device lines look like: [AVFoundation indev @ ...] [0] FaceTime HD Camera
            QRegularExpression devRegex("\\[(\\d+)\\]\\s+(.+)");
            QRegularExpressionMatch match = devRegex.match(line);
            if (match.hasMatch()) {
                QString devIndex = match.captured(1);
                QString devName  = match.captured(2).trimmed();
                if (inVideo) {
                    m_videoCombo->addItem(devName, devIndex);
                } else if (inAudio) {
                    m_audioCombo->addItem(devName, devIndex);
                }
            }
        }
    }
    if (m_videoCombo->count() == 0) {
        m_videoCombo->addItem("FaceTime HD Camera", "0");
    }
    if (m_audioCombo->count() == 0) {
        m_audioCombo->addItem("Built-in Microphone", "0");
    }
#endif

    logMessage(QString("Scan complete. Found %1 camera(s) and %2 audio device(s).")
               .arg(m_videoCombo->count())
               .arg(m_audioCombo->count()));
}

void MainWindow::refreshDevices()
{
    discoverDevices();
}

void MainWindow::onConnectClicked()
{
    if (m_isStreaming) {
        stopStreaming();
    } else {
        startStreaming();
    }
}

void MainWindow::startStreaming()
{
    QString host = m_ipEdit->text().trimmed();
    int port = m_portSpin->value();
    QString name = m_nameEdit->text().trimmed();
    
    // Sanitize display name (alphanumeric and underscores only)
    name.replace(" ", "_");
    QString sanitizedName = "";
    for (int i = 0; i < name.length(); ++i) {
        QChar c = name.at(i);
        if (c.isLetterOrNumber() || c == '_') {
            sanitizedName.append(c);
        }
    }
    
    if (host.isEmpty() || sanitizedName.isEmpty()) {
        QMessageBox::warning(this, "Input Error", "Please enter a valid Broadcaster IP and Display Name.");
        return;
    }

    QString videoDev = m_videoCombo->currentData().toString();
    QString audioDev = m_audioCombo->currentData().toString();
    
    if (videoDev.isEmpty() && m_videoCombo->currentText() != "No cameras found") {
        videoDev = m_videoCombo->currentText();
    }
    if (audioDev.isEmpty()) {
        audioDev = m_audioCombo->currentText();
    }

    if (videoDev.isEmpty() || audioDev.isEmpty()) {
        QMessageBox::warning(this, "Input Error", "Please select a video and audio device.");
        return;
    }

    QStringList arguments;

#if defined(Q_OS_LINUX)
    // Linux video/audio capture command
    arguments << "-f" << "v4l2" << "-input_format" << "mjpeg" << "-video_size" << "1280x720" << "-framerate" << "30" << "-i" << videoDev;
    arguments << "-f" << "alsa" << "-i" << audioDev;
#elif defined(Q_OS_WIN)
    // Windows DirectShow capture command
    arguments << "-f" << "dshow" << "-video_size" << "1280x720" << "-framerate" << "30" << "-i" << QString("video=%1:audio=%2").arg(videoDev, audioDev);
#elif defined(Q_OS_MAC)
    // macOS AVFoundation capture command (index-based: video:audio)
    arguments << "-f" << "avfoundation" << "-video_size" << "1280x720" << "-framerate" << "30" << "-i" << QString("%1:%2").arg(videoDev, audioDev);
#endif

    // High quality, ultra low latency streaming parameters
    arguments << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-tune" << "zerolatency" << "-pix_fmt" << "yuv420p";
    arguments << "-c:a" << "aac" << "-ac" << "2" << "-ar" << "44100" << "-b:a" << "128k";
    arguments << "-f" << "mpegts";

    // SRT Connection URL with the publish StreamID (MediaMTX style)
    QString srtUrl = QString("srt://%1:%2?mode=caller&streamid=publish:%3&latency=120000").arg(host).arg(port).arg(sanitizedName);
    arguments << srtUrl;

    logMessage("Starting transmission process...");
    logMessage("Execute: ffmpeg " + arguments.join(" "));

    m_streamProcess->start("ffmpeg", arguments);
    if (!m_streamProcess->waitForStarted(2000)) {
        logMessage("Error: Failed to spawn FFmpeg. Is FFmpeg installed and in your PATH?");
        QMessageBox::critical(this, "Process Error", "Failed to launch FFmpeg encoder. Make sure FFmpeg is installed.");
        return;
    }

    m_isStreaming = true;
    m_connectButton->setText("Disconnect");
    m_connectButton->setStyleSheet("background-color: #f38ba8; color: #11111b; font-weight: bold;");
    
    // Disable parameters during stream
    m_ipEdit->setEnabled(false);
    m_portSpin->setEnabled(false);
    m_nameEdit->setEnabled(false);
    m_videoCombo->setEnabled(false);
    m_audioCombo->setEnabled(false);
    m_refreshButton->setEnabled(false);
}

void MainWindow::stopStreaming()
{
    if (!m_isStreaming) return;
    
    logMessage("Stopping transmission process...");
    
    m_streamProcess->terminate();
    if (!m_streamProcess->waitForFinished(3000)) {
        logMessage("FFmpeg did not stop gracefully, forcing kill...");
        m_streamProcess->kill();
        m_streamProcess->waitForFinished(3000);
    }

    m_isStreaming = false;
}

void MainWindow::onProcessReadyReadStandardError()
{
    // FFmpeg logs mostly output to stderr
    QString errOutput = QString::fromUtf8(m_streamProcess->readAllStandardError());
    QStringList lines = errOutput.split('\n');
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            // Filter out verbose frame logs to not clutter the UI text edit
            if (!trimmed.contains("frame=") && !trimmed.contains("fps=") && !trimmed.contains("size=")) {
                logMessage("FFmpeg: " + trimmed);
            }
        }
    }
}

void MainWindow::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_isStreaming = false;
    m_connectButton->setText("Connect & Stream");
    m_connectButton->setStyleSheet("background-color: #a6e3a1; color: #11111b; font-weight: bold;");
    
    // Re-enable parameters
    m_ipEdit->setEnabled(true);
    m_portSpin->setEnabled(true);
    m_nameEdit->setEnabled(true);
    m_videoCombo->setEnabled(true);
    m_audioCombo->setEnabled(true);
    m_refreshButton->setEnabled(true);

    logMessage(QString("Transmission process finished with exit code %1 (Status: %2)")
               .arg(exitCode)
               .arg(exitStatus == QProcess::NormalExit ? "Normal" : "Crashed"));
}

void MainWindow::logMessage(const QString &msg)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    m_logEdit->append(QString("[%1] %2").arg(timestamp, msg));
}
