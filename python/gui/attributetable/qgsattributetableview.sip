/************************************************************************
 * This file has been generated automatically from                      *
 *                                                                      *
 * src/gui/attributetable/qgsattributetableview.h                       *
 *                                                                      *
 * Do not edit manually ! Edit header and run scripts/sipify.pl again   *
 ************************************************************************/






class QgsAttributeTableView : QTableView
{
%Docstring
 \brief
 Provides a table view of features of a QgsVectorLayer.

 This can either be used as a standalone widget. QgsBrowser features a reference implementation.
 Or this can be used within the QgsDualView stacked widget.
%End

%TypeHeaderCode
#include "qgsattributetableview.h"
%End
  public:
    QgsAttributeTableView( QWidget *parent /TransferThis/ = 0 );

    virtual void setModel( QgsAttributeTableFilterModel *filterModel );

    void setFeatureSelectionManager( QgsIFeatureSelectionManager *featureSelectionManager /Transfer/ );
%Docstring
 setFeatureSelectionManager
 \param featureSelectionManager We will take ownership
%End

    virtual bool eventFilter( QObject *object, QEvent *event );
%Docstring
 This event filter is installed on the verticalHeader to intercept mouse press and release
 events. These are used to disable / enable live synchronisation with the map canvas selection
 which can be slow due to recurring canvas repaints.

 \param object The object which is the target of the event.
 \param event  The intercepted event

 :return: Returns always false, so the event gets processed
 :rtype: bool
%End

    void setAttributeTableConfig( const QgsAttributeTableConfig &config );
%Docstring
 Set the attribute table config which should be used to control
 the appearance of the attribute table.
.. versionadded:: 2.16
%End

  protected:

    virtual void mousePressEvent( QMouseEvent *event );

%Docstring
 Called for mouse press events on a table cell.
 Disables selection change for these events.

 \param event The mouse event
%End

    virtual void mouseReleaseEvent( QMouseEvent *event );

%Docstring
 Called for mouse release events on a table cell.
 Disables selection change for these events.

 \param event The mouse event
%End

    virtual void mouseMoveEvent( QMouseEvent *event );

%Docstring
 Called for mouse move events on a table cell.
 Disables selection change for these events.

 \param event The mouse event
%End

    virtual void keyPressEvent( QKeyEvent *event );

%Docstring
 Called for key press events
 Disables selection change by only pressing an arrow key

 \param event The mouse event
%End

    virtual void contextMenuEvent( QContextMenuEvent *event );

%Docstring
 \brief
 Is called when the context menu will be shown. Emits a willShowContextMenu() signal,
 so the menu can be populated by other parts of the application.

 \param event The associated event object.
%End

    virtual void closeEvent( QCloseEvent *event );

%Docstring
 Saves geometry to the settings on close
 \param event not used
%End

  signals:

    void willShowContextMenu( QMenu *menu, const QModelIndex &atIndex );
%Docstring
 \brief
 Is emitted, in order to provide a hook to add additional* menu entries to the context menu.

 \param menu     If additional QMenuItems are added, they will show up in the context menu.
 \param atIndex  The QModelIndex, to which the context menu belongs. Relative to the source model.
                 In most cases, this will be a QgsAttributeTableFilterModel
%End

    void columnResized( int column, int width );
%Docstring
 Emitted when a column in the view has been resized.
 \param column column index (starts at 0)
 \param width new width in pixel
.. versionadded:: 2.16
%End

    void finished();

  public slots:
    void repaintRequested( const QModelIndexList &indexes );
    void repaintRequested();
    virtual void selectAll();
    virtual void selectRow( int row );
    virtual void _q_selectRow( int row );

};

/************************************************************************
 * This file has been generated automatically from                      *
 *                                                                      *
 * src/gui/attributetable/qgsattributetableview.h                       *
 *                                                                      *
 * Do not edit manually ! Edit header and run scripts/sipify.pl again   *
 ************************************************************************/
