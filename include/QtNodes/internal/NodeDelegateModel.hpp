#pragma once

#include "Definitions.hpp"
#include "Export.hpp"
#include "NodeData.hpp"
#include "NodeStyle.hpp"
#include "Serializable.hpp"
#include <QtGui/QColor>
#include <QtWidgets/QWidget>

namespace QtNodes {

/**
 * Describes whether a node configuration is usable and defines a description message
 */
struct NodeValidationState
{
    enum class State : int {
        Valid = 0,   ///< All required inputs are present and correct.
        Warning = 1, ///< Some inputs are missing or questionable, processing may be unreliable.
        Error = 2,   ///< Inputs or settings are invalid, preventing successful computation.
    };
    bool isValid() { return _state == State::Valid; };
    QString const message() { return _stateMessage; }
    State state() { return _state; }

    State _state{State::Valid};
    QString _stateMessage{""};
};

/**
* Describes the node status, depending on its current situation
*/
enum class NodeProcessingStatus : int {
    NoStatus = 0,   ///< No processing status is shown in the Node UI.
    Updated = 1,    ///< Node is up to date; its outputs reflect the current inputs and parameters.
    Processing = 2, ///< Node is currently running a computation.
    Pending = 3,    ///< Node is out of date and waiting to be recomputed (e.g. manual/queued run).
    Empty = 4,      ///< Node has no valid input data; nothing to compute.
    Failed = 5,     ///< The last computation ended with an error.
    Partial = 6,    ///< Computation finished incompletely; only partial results are available.
};

class StyleCollection;

/**
 * The class wraps Node-specific data operations and propagates it to
 * the nesting DataFlowGraphModel which is a subclass of
 * AbstractGraphModel.
 * This class is the same what has been called NodeDataModel before v3.
 */
class NODE_EDITOR_PUBLIC NodeDelegateModel
    : public QObject
    , public Serializable
{
    Q_OBJECT

public:
    NodeDelegateModel();

    virtual ~NodeDelegateModel() = default;

    /// Name makes this model unique
    virtual QString name() const = 0;

    /// Caption is used in GUI
    virtual QString caption() const = 0;

    /// It is possible to hide caption in GUI
    virtual bool captionVisible() const { return true; }

    /// Port caption is used in GUI to label individual ports
    virtual QString portCaption(PortType, PortIndex) const { return QString(); }

    /// It is possible to hide port caption in GUI
    virtual bool portCaptionVisible(PortType, PortIndex) const { return false; }

    /// Nicknames can be assigned to nodes and shown in GUI
    virtual QString label() const { return QString(); }

    /// It is possible to hide the nickname in GUI
    virtual bool labelVisible() const { return true; }

    /// Controls whether the label can be edited or not
    virtual bool labelEditable() const { return false; }

    /// Validation State will default to Valid, but you can manipulate it by overriding in an inherited class
    virtual NodeValidationState validationState() const { return _nodeValidationState; }

    /// Returns the curent processing status
    virtual NodeProcessingStatus processingStatus() const { return _processingStatus; }

    /// Progress is used in GUI
    virtual QString progressValue() const { return _progressValue; }

public:
    QJsonObject save() const override;

    void load(QJsonObject const &) override;

    void setNodeProcessingStatus(NodeProcessingStatus status);

    void setValidationState(const NodeValidationState &validationState);

public:
    virtual unsigned int nPorts(PortType portType) const = 0;

    virtual NodeDataType dataType(PortType portType, PortIndex portIndex) const = 0;

    virtual ConnectionPolicy portConnectionPolicy(PortType, PortIndex) const;

    NodeStyle const &nodeStyle() const;

    void setNodeStyle(NodeStyle const &style);

    /// Convenience helper to change the node background color.
    void setBackgroundColor(QColor const &color);

    QPixmap processingStatusIcon() const;

    void setStatusIcon(NodeProcessingStatus status, const QPixmap &pixmap);

    void setStatusIconStyle(ProcessingIconStyle const &style);

    void setProgressValue(QString new_progress) { _progressValue = new_progress; }

public:
    virtual void setInData(std::shared_ptr<NodeData> nodeData, PortIndex const portIndex) = 0;

    virtual std::shared_ptr<NodeData> outData(PortIndex const port) = 0;

    /**
     * It is recommented to preform lazy initialization for the embedded widget
     * and create it inside this function, not in the constructor of the current
     * model.
     *
     * Our Model Registry is able to shortly instantiate models in order to call
     * the non-static `Model::name()`. If the embedded widget is allocated in the
     * constructor but not actually embedded into some QGraphicsProxyWidget,
     * we'll gonna have a dangling pointer.
     */
    virtual QWidget *embeddedWidget() = 0;

    virtual bool resizable() const { return false; }

    /**
     * Whether the node's geometry (size / embedded widget position / port
     * positions) can change when data arrives on an input port.
     *
     * Defaults to true, which keeps the historical behavior: the scene runs
     * the full onNodeUpdated() path (recomputeSize + updateQWidgetEmbedPos +
     * moveConnections) on every setInData().
     *
     * Models whose size is fixed regardless of incoming data (e.g. video
     * pipeline nodes that repaint their display in setInData()) should return
     * false. The scene then uses a repaint-only fast path on data arrival,
     * avoiding the expensive geometry recompute + connection move cascade.
     */
    virtual bool dataArrivalChangesGeometry() const { return true; }

    /**
     * Whether the node's BODY (boundary, caption, ports, validation state)
     * needs repainting when new data arrives on an existing connection.
     *
     * Widget content changes self-repaint via Qt — return false for nodes
     * whose body appearance does not depend on data.
     *
     * Defaults to true, which keeps the historical behavior: the scene calls
     * node->update() on every setInData() to repaint the node body.
     */
    virtual bool dataArrivalChangesWidget() const { return true; }

    bool frozen() const { return _frozen; }

    void setFrozenState(bool state) { _frozen = state; }

public Q_SLOTS:
    virtual void inputConnectionCreated(ConnectionId const &) {}
    virtual void inputConnectionDeleted(ConnectionId const &) {}
    virtual void outputConnectionCreated(ConnectionId const &) {}
    virtual void outputConnectionDeleted(ConnectionId const &) {}

Q_SIGNALS:
    /// Triggers the updates in the nodes downstream.
    void dataUpdated(PortIndex const index);

    /// Triggers the propagation of the empty data downstream.
    void dataInvalidated(PortIndex const index);

    void computingStarted();
    void computingFinished();

    void embeddedWidgetSizeUpdated();

    /// Request an update of the node's UI.
    /**
     * Emit this signal whenever some internal state change requires
     * the node to be repainted. The containing graph model will
     * propagate the update to the scene.
     */
    void requestNodeUpdate();

    /// Call this function before deleting the data associated with ports.

    /**
     * @brief Call this function before deleting the data associated with ports.
     * The function notifies the Graph Model and makes it remove and recompute the
     * affected connection addresses.
     */
    void portsAboutToBeDeleted(PortType const portType, PortIndex const first, PortIndex const last);

    /// Call this function when data and port moditications are finished.
    void portsDeleted();

    /**
     * @brief Call this function before inserting the data associated with ports.
     * The function notifies the Graph Model and makes it recompute the affected
     * connection addresses.
     */
    void portsAboutToBeInserted(PortType const portType,
                                PortIndex const first,
                                PortIndex const last);

    /// Call this function when data and port moditications are finished.
    void portsInserted();

private:
    NodeStyle _nodeStyle;

    bool _frozen{false};

    NodeValidationState _nodeValidationState;

    NodeProcessingStatus _processingStatus{NodeProcessingStatus::NoStatus};

    QString _progressValue{QString()};
};

} // namespace QtNodes

Q_DECLARE_METATYPE(QtNodes::NodeValidationState)
Q_DECLARE_METATYPE(QtNodes::NodeProcessingStatus)
