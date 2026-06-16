#pragma once

#include <QMainWindow>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QTextEdit>
#include <QLabel>
#include <QProcess>
#include <QSystemTrayIcon>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onConnectClicked();
    void onProcessReadyReadStandardError();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void refreshDevices();

private:
    void setupUi();
    void discoverDevices();
    void startStreaming();
    void stopStreaming();
    void logMessage(const QString &msg);

    // UI Elements
    QLineEdit *m_ipEdit;
    QSpinBox *m_portSpin;
    QLineEdit *m_nameEdit;
    QComboBox *m_videoCombo;
    QComboBox *m_audioCombo;
    QPushButton *m_connectButton;
    QPushButton *m_refreshButton;
    QTextEdit *m_logEdit;

    // Process & State
    QProcess *m_streamProcess;
    bool m_isStreaming;
};
