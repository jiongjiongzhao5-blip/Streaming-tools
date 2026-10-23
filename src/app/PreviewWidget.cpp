#include "PreviewWidget.h"

#include <QColor>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QSize>

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PreviewWidget::setImage(const QImage& image)
{
    image_ = image;
    update();
}

void PreviewWidget::clear()
{
    image_ = QImage();
    update();
}

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(24, 24, 24));

    if (image_.isNull()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, "未开始推流");
        return;
    }

    const QSize scaled = image_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2,
                              (height() - scaled.height()) / 2),
                       scaled);

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, image_);
}
