#pragma once

#include <QImage>
#include <QWidget>

class PreviewWidget : public QWidget {
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
};
