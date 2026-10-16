#pragma once
#include <QListWidget>
#include <QTimer>

namespace Party {
enum NodeRole { NodeActiveRole = Qt::UserRole + 1, NodeTypeRole, NodeDelayRole, NodeDisplayNameRole, NodeTestingRole, NodeTestErrorRole };
class NodeList : public QListWidget {
public:
    explicit NodeList(QWidget *parent = nullptr);
    void syncAnimation();
protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    QTimer m_animation;
};
}
