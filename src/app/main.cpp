#include "MainWindow.h"

#include <QApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    app.setApplicationName("LightLiveStreamer");
    app.setApplicationDisplayName("LightLiveStreamer");
    app.setOrganizationName("LightLiveStreamer");

    MainWindow window;
    window.show();

    return app.exec();
}
