#include "nodelist.h"
#include <QPainter>
#include <QResizeEvent>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QMouseEvent>

namespace Party {
namespace {
QString delayText(const QModelIndex &index)
{
    if (index.data(NodeTestingRole).toBool()) return QObject::tr("测速中");
    const int delay = index.data(NodeDelayRole).toInt();
    const auto error = index.data(NodeTestErrorRole).toString();
    if (delay < 0) return QObject::tr("未测速");
    if (delay > 0) return QString::number(delay) + " ms";
    return error.isEmpty() || error.contains("timeout", Qt::CaseInsensitive) || error.contains("deadline", Qt::CaseInsensitive)
        ? QObject::tr("超时") : QObject::tr("失败");
}
QRect statusRect(const QRect &itemRect, const QFont &font, const QModelIndex &index)
{
    const auto card = itemRect.adjusted(4, 4, -4, -4);
    const int width = qMin(card.width() - 24, qMax(56, QFontMetrics(font).horizontalAdvance(delayText(index)) + 12));
    return QRect(card.right() - 11 - width, card.top() + 40, width, 28);
}
class NodeDelegate : public QStyledItemDelegate {
public:
    explicit NodeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect card = option.rect.adjusted(4, 4, -4, -4);
        const auto &palette = option.palette;
        const bool active = index.data(NodeActiveRole).toBool();
        const bool testing = index.data(NodeTestingRole).toBool();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        QColor background = palette.color(QPalette::Base);
        QColor border = palette.color(QPalette::Mid);
        border.setAlpha(85);
        if (active || selected || hover) {
            QColor accent = palette.color(QPalette::Highlight);
            const qreal opacity = active ? 0.12 : (selected ? 0.08 : 0.04);
            background.setRedF(background.redF() * (1 - opacity) + accent.redF() * opacity);
            background.setGreenF(background.greenF() * (1 - opacity) + accent.greenF() * opacity);
            background.setBlueF(background.blueF() * (1 - opacity) + accent.blueF() * opacity);
            if (active || selected) border = accent;
        }
        painter->setPen(QPen(border, active ? 1.8 : 1));
        painter->setBrush(background);
        painter->drawRoundedRect(card, 8, 8);
        const int left = card.left() + 12;
        const int top = card.top() + 12;
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).paint(painter, QRect(left, top, 28, 21));
        auto font = option.font;
        font.setBold(active);
        painter->setFont(font);
        painter->setPen(palette.color(QPalette::Text));
        const QRect title(left + 38, top, card.width() - 64 - (active || testing ? 16 : 0), 22);
        painter->drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(index.data(NodeDisplayNameRole).toString(), Qt::ElideRight, title.width()));
        if (testing) {
            const QRectF circle(card.right() - 26, top + 3, 14, 14);
            auto color = palette.color(QPalette::Highlight);
            color.setAlpha(45);
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(color, 2));
            painter->drawEllipse(circle);
            painter->setPen(QPen(palette.color(QPalette::Highlight), 2, Qt::SolidLine, Qt::RoundCap));
            painter->drawArc(circle, (90 - parent()->property("node-spinner-angle").toInt()) * 16, -260 * 16);
        } else if (active) {
            painter->setPen(QPen(palette.color(QPalette::Highlight), 2.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const int x = card.right() - 21, y = top + 10;
            painter->drawLine(x - 4, y, x - 1, y + 3);
            painter->drawLine(x - 1, y + 3, x + 5, y - 4);
        }
        font.setBold(false);
        painter->setFont(font);
        QColor secondary = palette.color(QPalette::Text);
        secondary.setAlpha(150);
        painter->setPen(secondary);
        const QRect detail(left, top + 32, card.width() - 24, 20);
        const int delay = index.data(NodeDelayRole).toInt();
        const auto text = delayText(index);
        const int delayWidth = statusRect(option.rect, font, index).width();
        painter->drawText(detail.adjusted(0, 0, -delayWidth, 0), Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(index.data(NodeTypeRole).toString(), Qt::ElideRight, detail.width() - delayWidth));
        if (testing) painter->setPen(palette.color(QPalette::Highlight));
        else if (delay >= 0) painter->setPen(delay == 0 || delay > 500 ? QColor("#de645b") : (delay > 200 ? QColor("#c18725") : QColor("#21a675")));
        painter->drawText(detail, Qt::AlignRight | Qt::AlignVCenter, text);
        if ((option.state & QStyle::State_HasFocus) && selected) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(palette.color(QPalette::Highlight), 1, Qt::DotLine));
            painter->drawRoundedRect(card.adjusted(3, 3, -3, -3), 6, 6);
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        const auto *view = qobject_cast<QListView *>(parent());
        return view ? QSize(qMax(1, view->gridSize().width() - 1), 88) : QSize(220, 88);
    }
};
}
NodeList::NodeList(QWidget *parent) : QListWidget(parent)
{
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setMouseTracking(true);
    viewport()->setCursor(Qt::PointingHandCursor);
    setUniformItemSizes(false);
    setSpacing(0);
    setIconSize(QSize(28, 21));
    setItemDelegate(new NodeDelegate(this));
    setGridSize(QSize(220, 88));
    setProperty("node-spinner-angle", 0);
    m_animation.setInterval(40);
    connect(&m_animation, &QTimer::timeout, this, [this] {
        setProperty("node-spinner-angle", (property("node-spinner-angle").toInt() + 18) % 360);
        for (int row = 0; row < count(); ++row)
            if (item(row)->data(NodeTestingRole).toBool()) viewport()->update(visualItemRect(item(row)));
    });
}
void NodeList::syncAnimation()
{
    bool testing = false;
    for (int row = 0; row < count(); ++row) if (item(row)->data(NodeTestingRole).toBool()) { testing = true; break; }
    if (testing && isVisible()) { if (!m_animation.isActive()) m_animation.start(); }
    else m_animation.stop();
}
void NodeList::showEvent(QShowEvent *event) { QListWidget::showEvent(event); syncAnimation(); }
void NodeList::hideEvent(QHideEvent *event) { m_animation.stop(); QListWidget::hideEvent(event); }
QRect NodeList::delayRect(const QListWidgetItem *item) const
{
    if (!item) return {};
    auto font = item->font(); font.setBold(false);
    return statusRect(visualItemRect(item), font, indexFromItem(item));
}
void NodeList::mousePressEvent(QMouseEvent *event)
{
    m_delayGesture = false; m_suppressRelease = false;
    m_pressedDelay = QPersistentModelIndex();
    if (event->button() == Qt::LeftButton) {
        const auto *item = itemAt(event->pos());
        if (item && delayRect(item).contains(event->pos())) {
            m_delayGesture = true; m_pressedDelay = indexFromItem(item); event->accept(); return;
        }
    }
    QListWidget::mousePressEvent(event);
}
void NodeList::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_suppressRelease) { m_suppressRelease = false; event->accept(); return; }
    if (m_delayGesture) {
        m_delayGesture = false;
        const auto pressed = m_pressedDelay;
        m_pressedDelay = QPersistentModelIndex();
        const auto *item = itemAt(event->pos());
        if (pressed.isValid() && event->button() == Qt::LeftButton && item && indexFromItem(item) == pressed && delayRect(item).contains(event->pos()))
            emit delayClicked(item->data(Qt::UserRole).toString());
        event->accept(); return;
    }
    QListWidget::mouseReleaseEvent(event);
}
void NodeList::mouseDoubleClickEvent(QMouseEvent *event)
{
    const auto *item = itemAt(event->pos());
    if (event->button() == Qt::LeftButton && item && delayRect(item).contains(event->pos())) { m_suppressRelease = true; event->accept(); return; }
    QListWidget::mouseDoubleClickEvent(event);
}
void NodeList::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    // Qt 5's icon layout reserves scrollbar space even while the bar is hidden.
    // Use the same bounds so a nominal two-column grid cannot wrap to one column.
    const int frame = style()->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents)
        ? style()->pixelMetric(QStyle::PM_DefaultFrameWidth) * 2 : 0;
    const int width = qMax(1, maximumViewportSize().width()
                           - style()->pixelMetric(QStyle::PM_ScrollBarExtent) - frame - 2);
    const int columns = qMax(1, width / 200);
    const QSize grid(width / columns, 88);
    if (grid != gridSize()) {
        setGridSize(grid);
        doItemsLayout();
    }
}
}
