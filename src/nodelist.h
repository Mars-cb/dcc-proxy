#pragma once
#include <QListWidget>
#include <QTimer>
#include <QPersistentModelIndex>

namespace Party {
enum NodeRole { NodeActiveRole = Qt::UserRole + 1, NodeTypeRole, NodeDelayRole, NodeDisplayNameRole, NodeTestingRole, NodeTestErrorRole };
class NodeList : public QListWidget {
    Q_OBJECT
public:
    explicit NodeList(QWidget *parent = nullptr);
    void syncAnimation();
    QRect delayRect(const QListWidgetItem *item) const;
signals:
    void delayClicked(const QString &name);
protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
    QTimer m_animation;
    QPersistentModelIndex m_pressedDelay;
    bool m_delayGesture = false;
    bool m_suppressRelease = false;
};
}
