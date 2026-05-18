#include "NodeGraphicsObject.hpp"
#include "AbstractGraphModel.hpp"
#include "AbstractNodeGeometry.hpp"
#include "AbstractNodePainter.hpp"
#include "BasicGraphicsScene.hpp"
#include "ConnectionGraphicsObject.hpp"
#include "ConnectionIdUtils.hpp"
#include "NodeConnectionInteraction.hpp"
#include "NodeDelegateModel.hpp"
#include "NodeGroup.hpp"
#include "StyleCollection.hpp"
#include "UndoCommands.hpp"
#include <QString>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#include <QtWidgets/QGraphicsEffect>
#include <QtWidgets/QtWidgets>

#include <cstdlib>

namespace QtNodes {

namespace {

void bringDetachedWindowToFront(QWidget *detachedWidget)
{
    if (!detachedWidget || !detachedWidget->isVisible())
        return;

    QWidget *topLevel = detachedWidget->window();
    if (!topLevel)
        return;

    if (topLevel->windowState().testFlag(Qt::WindowMinimized))
        topLevel->showNormal();

    Qt::WindowFlags const flags = topLevel->windowFlags();

    topLevel->setWindowFlags(flags & ~Qt::WindowStaysOnTopHint);
    topLevel->show();

    topLevel->setWindowFlags(flags | Qt::WindowStaysOnTopHint);
    topLevel->show();

    detachedWidget->raise();
    topLevel->raise();

    if (QWindow *handle = topLevel->windowHandle())
        handle->raise();
}

} // namespace

NodeGraphicsObject::NodeGraphicsObject(BasicGraphicsScene &scene, NodeId nodeId)
    : _nodeId(nodeId)
    , _graphModel(scene.graphModel())
    , _nodeState(*this)
    , _locked(false)
    , _draggingIntoGroup(false)
    , _possibleGroup(nullptr)
    , _originalGroupSize()
    , _proxyWidget(nullptr)
{
    scene.addItem(this);

    setFlag(QGraphicsItem::ItemDoesntPropagateOpacityToChildren, true);
    setFlag(QGraphicsItem::ItemIsFocusable, true);

    setLockedState();

    setCacheMode(QGraphicsItem::DeviceCoordinateCache);

    QJsonObject nodeStyleJson = _graphModel.nodeData(_nodeId, NodeRole::Style).toJsonObject();

    NodeStyle nodeStyle(nodeStyleJson);

    if (nodeStyle.ShadowEnabled) {
        auto effect = new QGraphicsDropShadowEffect;
        effect->setOffset(4, 4);
        effect->setBlurRadius(20);
        effect->setColor(nodeStyle.ShadowColor);

        setGraphicsEffect(effect);
    }

    setOpacity(nodeStyle.Opacity);

    setAcceptHoverEvents(true);

    setZValue(0);

    embedQWidget();

    nodeScene()->nodeGeometry().recomputeSize(_nodeId);

    QPointF const pos = _graphModel.nodeData<QPointF>(_nodeId, NodeRole::Position);

    setPos(pos);

    connect(&_graphModel, &AbstractGraphModel::nodeFlagsUpdated, this, [this](NodeId const nodeId) {
        if (_nodeId == nodeId)
            setLockedState();
    });
}

AbstractGraphModel &NodeGraphicsObject::graphModel() const
{
    return _graphModel;
}

BasicGraphicsScene *NodeGraphicsObject::nodeScene() const
{
    return dynamic_cast<BasicGraphicsScene *>(scene());
}

void NodeGraphicsObject::updateQWidgetEmbedPos()
{
    if (_proxyWidget) {
        AbstractNodeGeometry &geometry = nodeScene()->nodeGeometry();
        _proxyWidget->setPos(geometry.widgetPosition(_nodeId));
    }
}

bool NodeGraphicsObject::hasWidget() const
{
    return _graphModel.nodeData(_nodeId, NodeRole::Widget).value<QWidget *>() != nullptr;
}

bool NodeGraphicsObject::isWidgetEmbedded() const
{
    return _proxyWidget != nullptr && _proxyWidget->widget() != nullptr;
}

void NodeGraphicsObject::setWidgetEmbedded(bool embed)
{
    auto *scenePtr = nodeScene();
    if (!scenePtr)
        return;

    auto *widget = _graphModel.nodeData(_nodeId, NodeRole::Widget).value<QWidget *>();
    if (!widget)
        return;

    AbstractNodeGeometry &geometry = scenePtr->nodeGeometry();

    if (embed) {
        if (isWidgetEmbedded())
            return;

        if (_proxyWidget) {
            _proxyWidget->setWidget(nullptr);
            delete _proxyWidget;
            _proxyWidget = nullptr;
        }

        widget->hide();
        widget->setWindowFlag(Qt::Window, false);

        _proxyWidget = new QGraphicsProxyWidget(this);
        _proxyWidget->setWidget(widget);
        _proxyWidget->setPreferredWidth(5);

        // The widget may need to be hidden before re-embedding, but QGraphicsProxyWidget
        // mirrors that explicit hidden state. Show it again once it is owned by the proxy.
        _proxyWidget->show();
        widget->show();

        geometry.recomputeSize(_nodeId);

        if (widget->sizePolicy().verticalPolicy() & QSizePolicy::ExpandFlag) {
            unsigned int widgetHeight = geometry.size(_nodeId).height()
                                        - geometry.captionRect(_nodeId).height();
            _proxyWidget->setMinimumHeight(widgetHeight);
        }

        updateQWidgetEmbedPos();
        _proxyWidget->setOpacity(1.0);
        _proxyWidget->setFlag(QGraphicsItem::ItemIgnoresParentOpacity);
    } else {
        if (!isWidgetEmbedded())
            return;

        QSize embeddedSize;
        if (_proxyWidget)
            embeddedSize = _proxyWidget->size().toSize();

        QWidget *detachedWidget = _proxyWidget->widget();
        _proxyWidget->setWidget(nullptr);
        delete _proxyWidget;
        _proxyWidget = nullptr;

        if (detachedWidget) {
            Qt::WindowFlags topLevelFlags = Qt::Window
                                            | Qt::WindowTitleHint
                                            | Qt::WindowSystemMenuHint
                                            | Qt::WindowMinMaxButtonsHint
                                            | Qt::WindowStaysOnTopHint
                                            | Qt::WindowCloseButtonHint;
            detachedWidget->setParent(nullptr, topLevelFlags);
            detachedWidget->setWindowModality(Qt::NonModal);
            detachedWidget->setAttribute(Qt::WA_DeleteOnClose, false);
            detachedWidget->setAttribute(Qt::WA_DontShowOnScreen, false);
            detachedWidget->setWindowState(Qt::WindowNoState);

            auto const caption = _graphModel.nodeData<QString>(_nodeId, NodeRole::Caption);
            if (!caption.isEmpty())
                detachedWidget->setWindowTitle(caption);

            QSize targetSize = embeddedSize.isValid() ? embeddedSize : detachedWidget->size();
            if (!targetSize.isValid() || targetSize.width() <= 0 || targetSize.height() <= 0) {
                targetSize = detachedWidget->sizeHint().expandedTo(detachedWidget->minimumSizeHint());
            }
            if (!targetSize.isValid() || targetSize.width() <= 0 || targetSize.height() <= 0)
                targetSize = QSize(420, 260);

            QScreen *screen = nullptr;
            auto const sceneViews = scenePtr->views();
            QGraphicsView *hostView = nullptr;
            if (!sceneViews.isEmpty() && sceneViews.front()) {
                hostView = sceneViews.front();
                QWidget *hostWindow = hostView->window();
                if (hostWindow && hostWindow->windowHandle())
                    screen = hostWindow->windowHandle()->screen();
            }
            if (!screen)
                screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            QRect const available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

            if (targetSize.width() > available.width() || targetSize.height() > available.height()) {
                int fitW = available.width() - 32;
                int fitH = available.height() - 32;
                if (fitW < 120)
                    fitW = available.width();
                if (fitH < 90)
                    fitH = available.height();
                targetSize = QSize(fitW, fitH);
            }

            detachedWidget->resize(targetSize);

            // Prefer placing detached windows near the node location in the active view.
            QPoint targetPos;
            bool hasNodeAnchor = false;
            if (hostView && hostView->viewport()) {
                QPoint const nodeInView = hostView->mapFromScene(mapToScene(boundingRect().topLeft()));
                if (hostView->viewport()->rect().contains(nodeInView)) {
                    targetPos = hostView->viewport()->mapToGlobal(nodeInView + QPoint(24, 24));
                    hasNodeAnchor = true;
                }
            }

            if (!hasNodeAnchor) {
                targetPos = available.center() - QPoint(targetSize.width() / 2, targetSize.height() / 2);
            }

            // Small cascade offset avoids exact overlap when de-embedding several nodes in a row.
            static int detachCascadeIndex = 0;
            int const cascadeStep = 22;
            int const cascadeSlots = 6;
            int const cascadeOffset = (detachCascadeIndex % cascadeSlots) * cascadeStep;
            targetPos += QPoint(cascadeOffset, cascadeOffset);
            detachCascadeIndex = (detachCascadeIndex + 1) % cascadeSlots;

            int maxX = available.right() - targetSize.width() + 1;
            int maxY = available.bottom() - targetSize.height() + 1;
            if (targetPos.x() > maxX)
                targetPos.setX(maxX);
            if (targetPos.y() > maxY)
                targetPos.setY(maxY);
            if (targetPos.x() < available.left())
                targetPos.setX(available.left());
            if (targetPos.y() < available.top())
                targetPos.setY(available.top());

            detachedWidget->move(targetPos);
            detachedWidget->showNormal();
            detachedWidget->raise();
            detachedWidget->activateWindow();

            qDebug() << "DEEMBED widget:" << detachedWidget->metaObject()->className()
                     << "visible=" << detachedWidget->isVisible()
                     << "hidden=" << detachedWidget->isHidden()
                     << "geom=" << detachedWidget->geometry()
                     << "sizeHint=" << detachedWidget->sizeHint()
                     << "flags=" << detachedWidget->windowFlags()
                     << "state=" << detachedWidget->windowState();
        }
    }

    geometry.recomputeSize(_nodeId);
    update();
    moveConnections();
}

void NodeGraphicsObject::embedQWidget()
{
    setWidgetEmbedded(true);
}

void NodeGraphicsObject::setLockedState()
{
    NodeFlags flags = _graphModel.nodeFlags(_nodeId);

    bool const locked = flags.testFlag(NodeFlag::Locked);

    setFlag(QGraphicsItem::ItemIsMovable, !locked);
    setFlag(QGraphicsItem::ItemIsSelectable, !locked);
    setFlag(QGraphicsItem::ItemSendsScenePositionChanges, !locked);
}

QRectF NodeGraphicsObject::boundingRect() const
{
    AbstractNodeGeometry &geometry = nodeScene()->nodeGeometry();
    return geometry.boundingRect(_nodeId);
    //return NodeGeometry(_nodeId, _graphModel, nodeScene()).boundingRect();
}

void NodeGraphicsObject::setGeometryChanged()
{
    prepareGeometryChange();
}

void NodeGraphicsObject::setNodeGroup(std::shared_ptr<NodeGroup> group)
{
    _nodeGroup = group;
}

void NodeGraphicsObject::moveConnections() const
{
    auto const &connected = _graphModel.allConnectionIds(_nodeId);

    for (auto &cnId : connected) {
        auto cgo = nodeScene()->connectionGraphicsObject(cnId);

        if (cgo)
            cgo->move();
    }
}

void NodeGraphicsObject::reactToConnection(ConnectionGraphicsObject const *cgo)
{
    _nodeState.storeConnectionForReaction(cgo);

    update();
}

void NodeGraphicsObject::paint(QPainter *painter, QStyleOptionGraphicsItem const *option, QWidget *)
{
    QString tooltip;
    QVariant var = _graphModel.nodeData(_nodeId, NodeRole::ValidationState);
    if (var.canConvert<NodeValidationState>()) {
        auto state = var.value<NodeValidationState>();
        if (state._state != NodeValidationState::State::Valid) {
            tooltip = state._stateMessage;
        }
    }
    setToolTip(tooltip);

    painter->setClipRect(option->exposedRect);

    nodeScene()->nodePainter().paint(painter, *this);
}

QVariant NodeGraphicsObject::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == ItemScenePositionHasChanged && scene()) {
        moveConnections();
    }

    return QGraphicsObject::itemChange(change, value);
}

void NodeGraphicsObject::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    if (_locked) {
        nodeScene()->clearSelection();
        return;
    }

    AbstractNodeGeometry &geometry = nodeScene()->nodeGeometry();

    for (PortType portToCheck : {PortType::In, PortType::Out}) {
        QPointF nodeCoord = sceneTransform().inverted().map(event->scenePos());

        PortIndex const portIndex = geometry.checkPortHit(_nodeId, portToCheck, nodeCoord);

        if (portIndex == InvalidPortIndex)
            continue;

        auto const &connected = _graphModel.connections(_nodeId, portToCheck, portIndex);

        // Start dragging existing connection.
        if (!connected.empty() && portToCheck == PortType::In) {
            auto const &cnId = *connected.begin();

            // Need ConnectionGraphicsObject

            NodeConnectionInteraction interaction(*this,
                                                  *nodeScene()->connectionGraphicsObject(cnId),
                                                  *nodeScene());

            if (_graphModel.detachPossible(cnId))
                interaction.disconnect(portToCheck);
        } else // initialize new Connection
        {
            if (portToCheck == PortType::Out) {
                auto const outPolicy = _graphModel
                                           .portData(_nodeId,
                                                     portToCheck,
                                                     portIndex,
                                                     PortRole::ConnectionPolicyRole)
                                           .value<ConnectionPolicy>();

                if (!connected.empty() && outPolicy == ConnectionPolicy::One) {
                    for (auto &cnId : connected) {
                        _graphModel.deleteConnection(cnId);
                    }
                }
            } // if port == out

            ConnectionId const incompleteConnectionId = makeIncompleteConnectionId(_nodeId,
                                                                                   portToCheck,
                                                                                   portIndex);

            // From the moment of creation a draft connection
            // grabs the mouse events and waits for the mouse button release
            nodeScene()->makeDraftConnection(incompleteConnectionId);
        }
    }

    if (_graphModel.nodeFlags(_nodeId) & NodeFlag::Resizable) {
        auto pos = event->pos();
        bool const hit = geometry.resizeHandleRect(_nodeId).contains(QPoint(pos.x(), pos.y()));
        _nodeState.setResizing(hit);
    }

    QGraphicsObject::mousePressEvent(event);

    if (isSelected()) {
        Q_EMIT nodeScene()->nodeSelected(_nodeId);
    }
}

void NodeGraphicsObject::mouseMoveEvent(QGraphicsSceneMouseEvent *event)
{
    // Deselect all other items after this one is selected.
    // Unless we press a CTRL button to add the item to the selected group before
    // starting moving.
    if (!isSelected()) {
        if (!event->modifiers().testFlag(Qt::ControlModifier))
            scene()->clearSelection();

        setSelected(true);
    }

    if (_nodeState.resizing()) {
        auto diff = event->pos() - event->lastPos();

        if (auto w = _graphModel.nodeData<QWidget *>(_nodeId, NodeRole::Widget)) {
            prepareGeometryChange();
            auto oldSize = w->size();

            oldSize += QSize(diff.x(), diff.y());

            w->resize(oldSize);

            AbstractNodeGeometry &geometry = nodeScene()->nodeGeometry();

            // Passes the new size to the model.
            geometry.recomputeSize(_nodeId);

            update();

            moveConnections();

            event->accept();
        }
    } else {
        QGraphicsObject::mouseMoveEvent(event);

        if (event->lastPos() != event->pos()) {
            auto diff = event->pos() - event->lastPos();
            if (nodeScene()->groupingEnabled()) {
                if (auto nodeGroup = _nodeGroup.lock(); nodeGroup) {
                    nodeGroup->groupGraphicsObject().moveConnections();
                    if (nodeGroup->groupGraphicsObject().locked()) {
                        nodeGroup->groupGraphicsObject().moveNodes(diff);
                    }
                } else {
                    moveConnections();
                    // if it intersects with a group, expand group
                    QList<QGraphicsItem *> overlapItems = collidingItems();
                    for (auto &item : overlapItems) {
                        auto ggo = qgraphicsitem_cast<GroupGraphicsObject *>(item);
                        if (ggo != nullptr) {
                            if (!ggo->locked()) {
                                if (!_draggingIntoGroup) {
                                    _draggingIntoGroup = true;
                                    _possibleGroup = ggo;
                                    _originalGroupSize = _possibleGroup->mapRectToScene(ggo->rect());
                                    _possibleGroup->setPossibleChild(this);
                                    break;
                                } else {
                                    if (ggo == _possibleGroup) {
                                        if (!boundingRect().intersects(
                                                mapRectFromScene(_originalGroupSize))) {
                                            _draggingIntoGroup = false;
                                            _originalGroupSize = QRectF();
                                            _possibleGroup->unsetPossibleChild();
                                            _possibleGroup = nullptr;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                moveConnections();
            }
        }
        event->ignore();
    }

    QRectF r = nodeScene()->sceneRect();

    r = r.united(mapToScene(boundingRect()).boundingRect());

    nodeScene()->setSceneRect(r);
}

void NodeGraphicsObject::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
    _nodeState.setResizing(false);

    QGraphicsObject::mouseReleaseEvent(event);

    // position connections precisely after fast node move
    moveConnections();

    if (nodeScene()->groupingEnabled() && _draggingIntoGroup && _possibleGroup
        && _nodeGroup.expired()) {
        nodeScene()->addNodeToGroup(_nodeId, _possibleGroup->group().id());
        _possibleGroup->unsetPossibleChild();
        _draggingIntoGroup = false;
        _originalGroupSize = QRectF();
        _possibleGroup = nullptr;
    }

    nodeScene()->nodeClicked(_nodeId);
}

void NodeGraphicsObject::hoverEnterEvent(QGraphicsSceneHoverEvent *event)
{
    // bring all the colliding nodes to background
    QList<QGraphicsItem *> overlapItems = collidingItems();

    for (QGraphicsItem *item : overlapItems) {
        if (auto group = qgraphicsitem_cast<GroupGraphicsObject *>(item)) {
            Q_UNUSED(group);
            continue;
        }

        if (item->zValue() > 0.0) {
            item->setZValue(0.0);
        }
    }

    // bring this node forward
    setZValue(1.0);

    if (!isWidgetEmbedded()) {
        auto *detachedWidget = _graphModel.nodeData(_nodeId, NodeRole::Widget).value<QWidget *>();
        bringDetachedWindowToFront(detachedWidget);

    }

    _nodeState.setHovered(true);

    update();

    Q_EMIT nodeScene()->nodeHovered(_nodeId, event->screenPos());

    event->accept();
}

void NodeGraphicsObject::hoverLeaveEvent(QGraphicsSceneHoverEvent *event)
{
    _nodeState.setHovered(false);

    setZValue(0.0);

    update();

    Q_EMIT nodeScene()->nodeHoverLeft(_nodeId);

    event->accept();
}

void NodeGraphicsObject::hoverMoveEvent(QGraphicsSceneHoverEvent *event)
{
    auto pos = event->pos();

    //NodeGeometry geometry(_nodeId, _graphModel, nodeScene());
    AbstractNodeGeometry &geometry = nodeScene()->nodeGeometry();

    if ((_graphModel.nodeFlags(_nodeId) | NodeFlag::Resizable)
        && geometry.resizeHandleRect(_nodeId).contains(QPoint(pos.x(), pos.y()))) {
        setCursor(QCursor(Qt::SizeFDiagCursor));
    } else {
        setCursor(QCursor());
    }

    event->accept();
}

void NodeGraphicsObject::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event)
{
    QGraphicsItem::mouseDoubleClickEvent(event);

    Q_EMIT nodeScene()->nodeDoubleClicked(_nodeId);
}

void NodeGraphicsObject::contextMenuEvent(QGraphicsSceneContextMenuEvent *event)
{
    Q_EMIT nodeScene()->nodeContextMenu(_nodeId, mapToScene(event->pos()));
    event->accept();
}

void NodeGraphicsObject::lock(bool locked)
{
    _locked = locked;

    setFlag(QGraphicsItem::ItemIsFocusable, !locked);
    setFlag(QGraphicsItem::ItemIsSelectable, !locked);
}

QJsonObject NodeGraphicsObject::save() const
{
    QJsonObject nodeJson = _graphModel.saveNode(_nodeId);
    if (nodeJson.isEmpty()) {
        nodeJson["id"] = QString::number(_nodeId);
        QJsonObject obj;
        obj["x"] = pos().x();
        obj["y"] = pos().y();
        nodeJson["position"] = obj;
    }

    return nodeJson;
}
} // namespace QtNodes
