/***************************************************************************
                         qgslayoutmodel.cpp
                         ------------------
    begin                : October 2017
    copyright            : (C) 2017 by Nyall Dawson
    email                : nyall dot dawson at gmail dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgslayoutmodel.h"

#include "qgsapplication.h"
#include "qgslayout.h"
#include "qgslayoutitemgroup.h"
#include "qgslayoutitemgroupundocommand.h"
#include "qgslayoutundostack.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "qgsproject.h"

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QIODevice>
#include <QIcon>
#include <QMimeData>
#include <QSettings>
#include <QString>

#include "moc_qgslayoutmodel.cpp"

using namespace Qt::StringLiterals;

QgsLayoutModel::QgsLayoutModel( QgsLayout *layout, QObject *parent )
  : QAbstractItemModel( parent )
  , mLayout( layout )
{}

QgsLayoutItem *QgsLayoutModel::itemFromIndex( const QModelIndex &index ) const
{
  //try to return the QgsLayoutItem corresponding to a QModelIndex
  if ( !index.isValid() )
  {
    return nullptr;
  }

  //an index belonging to some other model carries a foreign internal pointer,
  //which the cast below would happily hand back as a QgsLayoutItem *
  if ( index.model() != this )
  {
    return nullptr;
  }

  // Internal pointer is the QgsLayoutItem * (or nullptr for the root sentinel
  // at top-level row 0). The static_cast naturally yields nullptr for that case.
  QgsLayoutItem *item = static_cast<QgsLayoutItem *>( index.internalPointer() );

  //dropMimeData(), QgsLayout::groupItems() and rebuildSceneItemList() all reset
  //the model wholesale, and an index held across such a reset still points at
  //whatever the item used to be. Every index this model creates takes its
  //internal pointer from mItemsInScene - index() reads topLevelItemsInScene()
  //and childItemsInScene(), parent() reads the same two lists, and both are
  //filtered views of mItemsInScene - so a pointer which is no longer in that
  //list belongs to an item the model has already let go of, and dereferencing
  //it is a use after free
  if ( item && !mItemsInScene.contains( item ) )
  {
    return nullptr;
  }

  return item;
}

void QgsLayoutModel::emitModelReset()
{
  QElapsedTimer t; t.start();
  const int top = topLevelItemsInScene().size();
  const int scene = mItemsInScene.size();
  beginResetModel();
  endResetModel();
  QgsMessageLog::logMessage(
    QStringLiteral( "emitModelReset: top=%1 scene=%2 elapsed=%3ms" )
      .arg( top ).arg( scene ).arg( t.elapsed() ),
    QStringLiteral( "LayoutPerf" ), Qgis::MessageLevel::Info );
}

QList<QgsLayoutItem *> QgsLayoutModel::topLevelItemsInScene() const
{
  QList<QgsLayoutItem *> result;
  result.reserve( mItemsInScene.size() );
  for ( QgsLayoutItem *item : mItemsInScene )
  {
    if ( !item->parentGroup() )
      result.append( item );
  }
  return result;
}

QList<QgsLayoutItem *> QgsLayoutModel::childItemsInScene( QgsLayoutItemGroup *group ) const
{
  QList<QgsLayoutItem *> result;
  if ( !group )
    return result;
  // Honor the group's local z-stack (mItems order) so reorderItemUp/Down
  // is reflected in the tree. Only include items currently in the scene.
  //
  // The parentGroup() check is load-bearing, not defensive: this walks the
  // group's own mItems list, while parent() and topLevelItemsInScene() derive
  // the hierarchy from the item's mParentGroupUuid instead. Undoing a
  // drag-to-reparent restores that uuid via readPropertiesFromElement(), but
  // QgsLayoutItemGroup::finalizeRestoreFromXml() only ever ADDS members and
  // never drops omitted ones, so mItems can still claim an item whose
  // parentGroup() is now null or a different group. Trusting mItems alone
  // makes the same item reachable through two QModelIndex paths and breaks
  // parent( index( r, c, p ) ) == p — the same QAbstractItemModel invariant
  // whose violation caused the stack overflow fixed in d86d3e5c2f.
  const QList<QgsLayoutItem *> groupItems = group->items();
  for ( QgsLayoutItem *item : groupItems )
  {
    if ( item && item->parentGroup() == group && mItemsInScene.contains( item ) )
      result.append( item );
  }
  return result;
}

QList<QgsLayoutItem *> QgsLayoutModel::prospectiveTopLevelItems() const
{
  //mirrors refreshItemsInScene() followed by topLevelItemsInScene(), but reads
  //the z-order list directly so that it can be called before the scene item
  //cache has been rebuilt
  QList<QgsLayoutItem *> result;
  result.reserve( mItemZList.size() );
  const QList< QGraphicsItem * > items = mLayout->items();
  for ( QgsLayoutItem *item : mItemZList )
  {
    if ( item && item->type() != QgsLayoutItemRegistry::LayoutPage && items.contains( item ) && !item->parentGroup() )
      result.append( item );
  }
  return result;
}

void QgsLayoutModel::refreshAfterZOrderMove( QgsLayoutItem *item )
{
  //the tree exposes a group's members in that group's own local order, which a
  //global restack never touches, and moving a group member elsewhere in the
  //z-order list leaves the relative order of the top level items alone. So the
  //only row which can change here belongs to a top level item, and it can only
  //move within the top level.
  if ( !item || item->parentGroup() )
  {
    refreshItemsInScene();
    return;
  }

  //top level rows are shifted by one by the root sentinel at row 0
  const int oldRow = static_cast< int >( topLevelItemsInScene().indexOf( item ) ) + 1;
  const int newRow = static_cast< int >( prospectiveTopLevelItems().indexOf( item ) ) + 1;
  if ( oldRow == 0 || newRow == 0 || oldRow == newRow )
  {
    //not exposed by the tree before or after the move, or not actually moved
    refreshItemsInScene();
    return;
  }

  //beginMoveRows() interprets destinationChild with the source rows still in
  //place, so a move to a later row under the same parent has to account for the
  //row which is taken out first. Qt also requires destinationChild to fall
  //outside sourceFirst..sourceLast+1, which both branches satisfy.
  beginMoveRows( QModelIndex(), oldRow, oldRow, QModelIndex(), newRow > oldRow ? newRow + 1 : newRow );
  refreshItemsInScene();
  endMoveRows();
}

QModelIndex QgsLayoutModel::index( int row, int column, const QModelIndex &parent ) const
{
  if ( column < 0 || column >= columnCount() )
  {
    //column out of bounds
    return QModelIndex();
  }

  if ( !parent.isValid() )
  {
    if ( row == 0 )
    {
      // root sentinel — paper item placeholder, hidden by the items panel proxy
      return createIndex( row, column, nullptr );
    }

    const QList<QgsLayoutItem *> top = topLevelItemsInScene();
    if ( row >= 1 && row <= top.size() )
    {
      return createIndex( row, column, top.at( row - 1 ) );
    }
    return QModelIndex();
  }

  // parent must be a group for there to be children
  QgsLayoutItem *parentItem = itemFromIndex( parent );
  QgsLayoutItemGroup *group = qobject_cast<QgsLayoutItemGroup *>( parentItem );
  if ( !group )
    return QModelIndex();

  const QList<QgsLayoutItem *> children = childItemsInScene( group );
  if ( row >= 0 && row < children.size() )
  {
    return createIndex( row, column, children.at( row ) );
  }
  return QModelIndex();
}

void QgsLayoutModel::refreshItemsInScene()
{
  mItemsInScene.clear();

  const QList< QGraphicsItem * > items = mLayout->items();
  //filter paper items from list
  //TODO - correctly handle grouped item z order placement
  for ( QgsLayoutItem *item : std::as_const( mItemZList ) )
  {
    if ( item->type() != QgsLayoutItemRegistry::LayoutPage && items.contains( item ) )
    {
      mItemsInScene.push_back( item );
    }
  }
}

QModelIndex QgsLayoutModel::parent( const QModelIndex &index ) const
{
  if ( !index.isValid() )
    return QModelIndex();

  //itemFromIndex(), not a raw cast: this must not walk the parent chain of an
  //item which has already left the model
  QgsLayoutItem *item = itemFromIndex( index );
  if ( !item )
    return QModelIndex();

  QgsLayoutItemGroup *parentGroup = item->parentGroup();
  if ( !parentGroup )
    return QModelIndex();

  // Find parent's row in ITS parent's child list
  QgsLayoutItemGroup *grandparent = parentGroup->parentGroup();
  int parentRow = -1;
  if ( grandparent )
  {
    const QList<QgsLayoutItem *> siblings = childItemsInScene( grandparent );
    parentRow = siblings.indexOf( parentGroup );
  }
  else
  {
    const QList<QgsLayoutItem *> top = topLevelItemsInScene();
    parentRow = top.indexOf( parentGroup );
    if ( parentRow >= 0 )
      parentRow += 1; // shift past the root sentinel at top level
  }
  if ( parentRow < 0 )
    return QModelIndex();
  return createIndex( parentRow, 0, parentGroup );
}

int QgsLayoutModel::rowCount( const QModelIndex &parent ) const
{
  if ( !parent.isValid() )
  {
    // top-level rows = sentinel + items with no parent group
    return topLevelItemsInScene().size() + 1;
  }

  QgsLayoutItem *parentItem = itemFromIndex( parent );
  QgsLayoutItemGroup *group = qobject_cast<QgsLayoutItemGroup *>( parentItem );
  if ( !group )
    return 0;
  return childItemsInScene( group ).size();
}

int QgsLayoutModel::columnCount( const QModelIndex &parent ) const
{
  Q_UNUSED( parent )
  return 3;
}

QVariant QgsLayoutModel::data( const QModelIndex &index, int role ) const
{
  if ( !index.isValid() )
    return QVariant();

  QgsLayoutItem *item = itemFromIndex( index );
  if ( !item )
  {
    return QVariant();
  }

  switch ( role )
  {
    case Qt::DisplayRole:
      if ( index.column() == ItemId )
      {
        return item->displayName();
      }
      else
      {
        return QVariant();
      }

    case Qt::DecorationRole:
      if ( index.column() == ItemId )
      {
        return item->icon();
      }
      else
      {
        return QVariant();
      }

    case Qt::EditRole:
      if ( index.column() == ItemId )
      {
        return item->id();
      }
      else
      {
        return QVariant();
      }

    case Qt::UserRole:
      //store item uuid in userrole so we can later get the QModelIndex for a specific item
      return item->uuid();
    case Qt::UserRole + 1:
      //user role stores reference in column object
      return QVariant::fromValue( qobject_cast<QObject *>( item ) );

    case Qt::TextAlignmentRole:
      return static_cast<Qt::Alignment::Int>( Qt::AlignLeft & Qt::AlignVCenter );

    case Qt::CheckStateRole:
      switch ( index.column() )
      {
        case Visibility:
          //column 0 is visibility of item
          return item->isVisible() ? Qt::Checked : Qt::Unchecked;
        case LockStatus:
          //column 1 is locked state of item
          return item->isLocked() ? Qt::Checked : Qt::Unchecked;
        default:
          return QVariant();
      }

    default:
      return QVariant();
  }
}

bool QgsLayoutModel::setData( const QModelIndex &index, const QVariant &value, int role = Qt::EditRole )
{
  Q_UNUSED( role )

  if ( !index.isValid() )
    return false;

  QgsLayoutItem *item = itemFromIndex( index );
  if ( !item )
  {
    return false;
  }

  switch ( index.column() )
  {
    case Visibility:
      //first column is item visibility
      item->setVisibility( value.toBool() );
      return true;

    case LockStatus:
      //second column is item lock state
      item->setLocked( value.toBool() );
      return true;

    case ItemId:
      //last column is item id
      item->setId( value.toString() );
      return true;
  }

  return false;
}

QVariant QgsLayoutModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
  switch ( role )
  {
    case Qt::DisplayRole:
    {
      if ( section == ItemId )
      {
        return tr( "Item" );
      }
      return QVariant();
    }

    case Qt::DecorationRole:
    {
      if ( section == Visibility )
      {
        return QVariant::fromValue( QgsApplication::getThemeIcon( u"/mActionShowAllLayersGray.svg"_s ) );
      }
      else if ( section == LockStatus )
      {
        return QVariant::fromValue( QgsApplication::getThemeIcon( u"/lockedGray.svg"_s ) );
      }

      return QVariant();
    }

    case Qt::TextAlignmentRole:
      return static_cast<Qt::Alignment::Int>( Qt::AlignLeft & Qt::AlignVCenter );

    default:
      return QAbstractItemModel::headerData( section, orientation, role );
  }
}

Qt::DropActions QgsLayoutModel::supportedDropActions() const
{
  return Qt::MoveAction;
}

QStringList QgsLayoutModel::mimeTypes() const
{
  QStringList types;
  types << u"application/x-vnd.qgis.qgis.composeritemid"_s;
  return types;
}

QMimeData *QgsLayoutModel::mimeData( const QModelIndexList &indexes ) const
{
  QMimeData *mimeData = new QMimeData();
  QByteArray encodedData;

  QDataStream stream( &encodedData, QIODevice::WriteOnly );

  for ( const QModelIndex &index : indexes )
  {
    if ( index.isValid() && index.column() == ItemId )
    {
      QgsLayoutItem *item = itemFromIndex( index );
      if ( !item )
      {
        continue;
      }
      QString text = item->uuid();
      stream << text;
    }
  }

  mimeData->setData( u"application/x-vnd.qgis.qgis.composeritemid"_s, encodedData );
  return mimeData;
}

bool zOrderDescending( QgsLayoutItem *item1, QgsLayoutItem *item2 )
{
  return item1->zValue() > item2->zValue();
}

/**
 * Appends \a item, and for a group its whole subtree in local z-order, to \a result.
 * A group occupies a contiguous run of the global z-order list, so a group can only
 * be restacked by moving that whole run.
 */
static void appendItemAndDescendants( QgsLayoutItem *item, QList<QgsLayoutItem *> &result )
{
  if ( !item || result.contains( item ) )
  {
    return;
  }

  result.append( item );
  if ( QgsLayoutItemGroup *group = qobject_cast<QgsLayoutItemGroup *>( item ) )
  {
    const QList<QgsLayoutItem *> children = group->items();
    for ( QgsLayoutItem *child : children )
    {
      appendItemAndDescendants( child, result );
    }
  }
}

bool QgsLayoutModel::dropMimeData( const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent )
{
  Q_UNUSED( column ) //whole rows are moved, so the column under the cursor is irrelevant

  if ( action == Qt::IgnoreAction )
  {
    return true;
  }

  if ( !data->hasFormat( u"application/x-vnd.qgis.qgis.composeritemid"_s ) )
  {
    return false;
  }

  //a valid parent index means the items were dropped inside a group, anything
  //else is a drop at the top level of the layout
  QgsLayoutItemGroup *targetGroup = nullptr;
  if ( parent.isValid() )
  {
    targetGroup = qobject_cast<QgsLayoutItemGroup *>( itemFromIndex( parent ) );
    if ( !targetGroup )
    {
      //only groups can contain other items
      return false;
    }
  }

  QByteArray encodedData = data->data( u"application/x-vnd.qgis.qgis.composeritemid"_s );
  QDataStream stream( &encodedData, QIODevice::ReadOnly );
  QList<QgsLayoutItem *> droppedItems;

  while ( !stream.atEnd() )
  {
    QString text;
    stream >> text;
    QgsLayoutItem *item = mLayout->itemByUuid( text );
    if ( item )
    {
      droppedItems << item;
    }
  }

  if ( droppedItems.empty() )
  {
    //no dropped items
    return false;
  }

  //a group can never be dropped into itself or into one of its own descendants
  for ( QgsLayoutItemGroup *group = targetGroup; group; group = group->parentGroup() )
  {
    if ( droppedItems.contains( group ) )
    {
      return false;
    }
  }

  //an item whose enclosing group is being dropped too travels with that group,
  //so only the outermost dropped items are moved in their own right
  QList<QgsLayoutItem *> movedRoots;
  movedRoots.reserve( droppedItems.size() );
  for ( QgsLayoutItem *item : std::as_const( droppedItems ) )
  {
    bool ancestorDropped = false;
    for ( QgsLayoutItemGroup *group = item->parentGroup(); group; group = group->parentGroup() )
    {
      if ( droppedItems.contains( group ) )
      {
        ancestorDropped = true;
        break;
      }
    }
    if ( !ancestorDropped )
    {
      movedRoots << item;
    }
  }

  if ( movedRoots.empty() )
  {
    return false;
  }

  //first sort them by z-order, so their relative stacking survives the move
  std::sort( movedRoots.begin(), movedRoots.end(), zOrderDescending );

  QList<QgsLayoutItem *> movedItems;
  for ( QgsLayoutItem *item : std::as_const( movedRoots ) )
  {
    appendItemAndDescendants( item, movedItems );
  }

  //work out where the items land among their new siblings. This has to happen
  //before any parent relationship changes, because both sibling lists are
  //derived live from parentGroup()
  const QList<QgsLayoutItem *> siblings = targetGroup ? childItemsInScene( targetGroup ) : topLevelItemsInScene();
  const int siblingCount = static_cast< int >( siblings.size() );

  int siblingPos;
  if ( row < 0 )
  {
    //dropped straight onto a group: land on top of it, matching the layout
    //convention that items arriving in the stack arrive at the top. Dropped on
    //empty space instead: land at the bottom of the layout
    siblingPos = targetGroup ? 0 : siblingCount;
  }
  else
  {
    //top level rows are shifted by one by the root sentinel at row 0
    siblingPos = targetGroup ? row : row - 1;
  }
  if ( siblingPos < 0 )
    siblingPos = 0;
  if ( siblingPos > siblingCount )
    siblingPos = siblingCount;

  //the sibling the moved block ends up above, skipping siblings which are moving too
  QgsLayoutItem *insertBefore = nullptr;
  for ( int i = siblingPos; i < siblingCount; ++i )
  {
    if ( !movedRoots.contains( siblings.at( i ) ) )
    {
      insertBefore = siblings.at( i );
      break;
    }
  }

  //...and, when the block lands at the very bottom of a group, the last sibling
  //staying put, which the block has to be stacked below
  QgsLayoutItem *insertAfter = nullptr;
  if ( !insertBefore && targetGroup )
  {
    for ( QgsLayoutItem *sibling : siblings )
    {
      if ( !movedRoots.contains( sibling ) )
        insertAfter = sibling;
    }
  }

  //calculate position in the z-order list to drop items at
  int destPos = static_cast< int >( mItemZList.size() );
  if ( insertBefore )
  {
    destPos = static_cast< int >( mItemZList.indexOf( insertBefore ) );
  }
  else if ( insertAfter )
  {
    //below the anchor and everything the anchor itself contains
    QList<QgsLayoutItem *> anchorBlock;
    appendItemAndDescendants( insertAfter, anchorBlock );
    int lastPos = -1;
    for ( QgsLayoutItem *blockItem : std::as_const( anchorBlock ) )
    {
      const int pos = static_cast< int >( mItemZList.indexOf( blockItem ) );
      if ( pos > lastPos )
        lastPos = pos;
    }
    destPos = lastPos + 1;
  }
  else if ( targetGroup )
  {
    //nothing else left in the group, so sit directly below the group item
    destPos = static_cast< int >( mItemZList.indexOf( targetGroup ) ) + 1;
  }
  if ( destPos < 0 )
    destPos = static_cast< int >( mItemZList.size() );

  //record where the moved items sit now. This has to happen before any parent
  //relationship changes, and cannot be left to the item state commands: a
  //group's member list is restored additively by
  //QgsLayoutItemGroup::finalizeRestoreFromXml(), so replaying an item's XML can
  //put it back into the group it was dragged into but never take it out again
  QList< QgsLayoutItemReparentUndoCommand::Placement > beforePlacements;
  beforePlacements.reserve( movedRoots.size() );
  QList< QgsLayoutItemGroup * > sourceGroups;
  bool hierarchyChanged = static_cast< bool >( targetGroup );
  for ( QgsLayoutItem *item : std::as_const( movedRoots ) )
  {
    QgsLayoutItemGroup *oldGroup = item->parentGroup();
    if ( oldGroup != targetGroup )
      hierarchyChanged = true;

    QgsLayoutItemReparentUndoCommand::Placement placement;
    placement.itemUuid = item->uuid();
    if ( oldGroup )
    {
      placement.groupUuid = oldGroup->uuid();
      placement.index = static_cast< int >( oldGroup->items().indexOf( item ) );
      if ( !sourceGroups.contains( oldGroup ) )
        sourceGroups << oldGroup;
    }
    beforePlacements << placement;
  }

  mLayout->undoStack()->beginMacro( tr( "Move Items" ) );

  //index(), parent() and rowCount() are all computed live from parentGroup()
  //and from each group's member list, so the old structure cannot be held
  //still between beginMoveRows() and endMoveRows() while those lists are
  //rewritten. Reset the model instead, exactly as QgsLayout::groupItems()
  //does for the same reason - emitting granular row moves here would repeat
  //the mid-transaction signalling which already crashed this panel once.
  beginResetModel();

  for ( QgsLayoutItem *item : std::as_const( movedRoots ) )
  {
    if ( QgsLayoutItemGroup *oldGroup = item->parentGroup() )
      oldGroup->removeItem( item );
  }

  if ( targetGroup )
  {
    const QList<QgsLayoutItem *> remaining = targetGroup->items();
    int insertAt = insertBefore ? static_cast< int >( remaining.indexOf( insertBefore ) ) : -1;
    if ( insertAt < 0 )
      insertAt = static_cast< int >( remaining.size() );

    for ( QgsLayoutItem *item : std::as_const( movedRoots ) )
    {
      targetGroup->insertItem( item, insertAt );
      insertAt++;
    }
  }

  QList< QgsLayoutItemReparentUndoCommand::Placement > afterPlacements;
  afterPlacements.reserve( movedRoots.size() );
  for ( QgsLayoutItem *item : std::as_const( movedRoots ) )
  {
    QgsLayoutItemReparentUndoCommand::Placement placement;
    placement.itemUuid = item->uuid();
    if ( targetGroup )
    {
      placement.groupUuid = targetGroup->uuid();
      placement.index = static_cast< int >( targetGroup->items().indexOf( item ) );
    }
    afterPlacements << placement;
  }

  //calculate position to insert moved rows to
  int insertPos = destPos;
  for ( QgsLayoutItem *item : std::as_const( movedItems ) )
  {
    const int listPos = static_cast< int >( mItemZList.indexOf( item ) );
    if ( listPos == -1 )
    {
      //should be impossible
      continue;
    }

    if ( listPos < destPos )
    {
      insertPos--;
    }
  }

  //remove rows from list
  for ( QgsLayoutItem *item : std::as_const( movedItems ) )
  {
    mItemZList.removeOne( item );
  }

  if ( insertPos < 0 )
    insertPos = 0;
  if ( insertPos > mItemZList.size() )
    insertPos = static_cast< int >( mItemZList.size() );

  //insert items
  for ( QgsLayoutItem *item : std::as_const( movedItems ) )
  {
    mItemZList.insert( insertPos, item );
    insertPos++;
  }

  refreshItemsInScene();
  endResetModel();

  if ( hierarchyChanged )
  {
    mLayout->undoStack()->push( new QgsLayoutItemReparentUndoCommand( mLayout, beforePlacements, afterPlacements, tr( "Move Items" ) ) );
  }

  //now that the hierarchy half of the move is undoable the stacking half can be
  //too - undoing only the stacking used to leave items under the wrong group,
  //which is why these commands were suppressed for a re-parenting drop
  mLayout->updateZValues( true );

  //a group which has just lost its last member is as meaningless as one which
  //was ungrouped, and QgsLayout::ungroupItems() deletes the group in that case.
  //This runs after endResetModel() because removeLayoutItem() signals the
  //removal itself, which it may not do in the middle of a reset
  for ( QgsLayoutItemGroup *sourceGroup : std::as_const( sourceGroups ) )
  {
    if ( sourceGroup != targetGroup && sourceGroup->items().empty() )
      mLayout->removeLayoutItem( sourceGroup );
  }

  mLayout->undoStack()->endMacro();

  if ( hierarchyChanged )
  {
    if ( QgsProject *project = mLayout->project() )
      project->setDirty( true );
  }

  return true;
}

bool QgsLayoutModel::removeRows( int row, int count, const QModelIndex &parent )
{
  Q_UNUSED( count )
  if ( row >= rowCount( parent ) )
  {
    return false;
  }

  //do nothing - moves are handled by the dropMimeData method
  return true;
}

///@cond PRIVATE
void QgsLayoutModel::clear()
{
  //totally reset model
  beginResetModel();
  mItemZList.clear();
  refreshItemsInScene();
  endResetModel();
}

int QgsLayoutModel::zOrderListSize() const
{
  return mItemZList.size();
}

void QgsLayoutModel::rebuildZList()
{
  QList<QgsLayoutItem *> sortedList;
  //rebuild the item z order list based on the current zValues of items in the scene

  //get items in descending zValue order
  const QList<QGraphicsItem *> itemList = mLayout->items( Qt::DescendingOrder );
  for ( QGraphicsItem *item : itemList )
  {
    if ( QgsLayoutItem *layoutItem = dynamic_cast<QgsLayoutItem *>( item ) )
    {
      if ( layoutItem->type() != QgsLayoutItemRegistry::LayoutPage )
      {
        sortedList.append( layoutItem );
      }
    }
  }

  mItemZList = sortedList;
  rebuildSceneItemList();
}
///@endcond

void QgsLayoutModel::rebuildSceneItemList()
{
  //a position in mItemsInScene is only the model row while nothing is grouped:
  //once an item has a parent group its row is an index into that group's own
  //member list, and the top level rows skip it entirely, so the granular
  //signals below would describe rows which do not exist. Group membership can
  //also change wholesale between calls - this runs on every item added and at
  //the end of every project load or paste - which no sequence of row moves can
  //express. Reset instead, exactly as QgsLayout::groupItems() and
  //dropMimeData() do, and keep the granular path for the ungrouped case.
  bool hasGroups = false;
  for ( QgsLayoutItem *item : std::as_const( mItemZList ) )
  {
    if ( item && item->type() == QgsLayoutItemRegistry::LayoutGroup )
    {
      hasGroups = true;
      break;
    }
  }

  if ( hasGroups )
  {
    beginResetModel();
    refreshItemsInScene();
    endResetModel();
    return;
  }

  //step through the z list and rebuild the items in scene list,
  //emitting signals as required
  int row = 0;
  const QList< QGraphicsItem * > items = mLayout->items();
  for ( QgsLayoutItem *item : std::as_const( mItemZList ) )
  {
    if ( item->type() == QgsLayoutItemRegistry::LayoutPage || !items.contains( item ) )
    {
      //item not in scene, skip it
      continue;
    }

    int sceneListPos = mItemsInScene.indexOf( item );
    if ( sceneListPos == row )
    {
      //already in list in correct position, nothing to do
    }
    else if ( sceneListPos != -1 )
    {
      //in list, but in wrong spot
      beginMoveRows( QModelIndex(), sceneListPos + 1, sceneListPos + 1, QModelIndex(), row + 1 );
      mItemsInScene.removeAt( sceneListPos );
      mItemsInScene.insert( row, item );
      endMoveRows();
    }
    else
    {
      //needs to be inserted into list
      beginInsertRows( QModelIndex(), row + 1, row + 1 );
      mItemsInScene.insert( row, item );
      endInsertRows();
    }
    row++;
  }
}
///@cond PRIVATE
void QgsLayoutModel::addItemAtTop( QgsLayoutItem *item )
{
  mItemZList.push_front( item );
  refreshItemsInScene();
  item->setZValue( mItemZList.size() );
}

void QgsLayoutModel::removeItem( QgsLayoutItem *item )
{
  if ( !item )
  {
    //nothing to do
    return;
  }

  int pos = mItemZList.indexOf( item );
  if ( pos == -1 )
  {
    //item not in z list, nothing to do
    return;
  }

  //dropping a group takes its own row away and promotes every member it holds
  //to a top level row in the same breath, which no row removal can express.
  //
  //Only when it still HOLDS members, though. removeItem() is reached from
  //QgsLayoutItem::cleanup() (qgslayoutitem.cpp:96), which runs from
  //~QgsLayoutItem and from QgsLayoutItemGroup::cleanup() — and that cleanup
  //calls item->cleanup() on every member BEFORE QgsLayoutItem::cleanup() for
  //the group itself (qgslayoutitemgroup.cpp:53,58). So on the destructor path
  //the members are already out of the z list and there is nothing to promote;
  //resetting there would make every attached view re-query the model from
  //inside ~QgsLayoutItemGroup, against a half-destroyed object. An empty group
  //is an ordinary row removal, so fall through to it.
  QgsLayoutItemGroup *removedGroup = qobject_cast<QgsLayoutItemGroup *>( item );
  if ( removedGroup && !childItemsInScene( removedGroup ).isEmpty() )
  {
    beginResetModel();
    mItemZList.removeAt( pos );
    refreshItemsInScene();
    endResetModel();
    return;
  }

  //need to get QModelIndex of item
  QModelIndex itemIndex = indexForItem( item );
  if ( !itemIndex.isValid() )
  {
    //removing an item not in the scene (e.g., deleted item)
    //we need to remove it from the list, but don't need to call
    //beginRemoveRows or endRemoveRows since the item was not used by the model
    mItemZList.removeAt( pos );
    refreshItemsInScene();
    return;
  }

  //remove item from model. A grouped item is a row of its group, not of the
  //root, so the removal has to be announced against the item's own parent
  const QModelIndex parentIndex = itemIndex.parent();
  const int row = itemIndex.row();
  beginRemoveRows( parentIndex, row, row );
  mItemZList.removeAt( pos );
  refreshItemsInScene();
  endRemoveRows();
}

void QgsLayoutModel::setItemRemoved( QgsLayoutItem *item )
{
  if ( !item )
  {
    //nothing to do
    return;
  }

  int pos = mItemZList.indexOf( item );
  if ( pos == -1 )
  {
    //item not in z list, nothing to do
    return;
  }

  //a group leaving the scene takes the parent out from under its members:
  //parentGroup() resolves mParentGroupUuid against the scene, so they all
  //become top level rows at once. That is a restructure, not a row removal
  if ( qobject_cast<QgsLayoutItemGroup *>( item ) )
  {
    beginResetModel();
    mLayout->removeItem( item );
    refreshItemsInScene();
    endResetModel();
    return;
  }

  //need to get QModelIndex of item
  QModelIndex itemIndex = indexForItem( item );
  if ( !itemIndex.isValid() )
  {
    return;
  }

  //removing item. A grouped item is a row of its group, not of the root, so
  //the removal has to be announced against the item's own parent
  const QModelIndex parentIndex = itemIndex.parent();
  const int row = itemIndex.row();
  beginRemoveRows( parentIndex, row, row );
  mLayout->removeItem( item );
  refreshItemsInScene();
  endRemoveRows();
}

void QgsLayoutModel::updateItemDisplayName( QgsLayoutItem *item )
{
  if ( !item )
  {
    //nothing to do
    return;
  }

  //need to get QModelIndex of item
  QModelIndex itemIndex = indexForItem( item, ItemId );
  if ( !itemIndex.isValid() )
  {
    return;
  }

  //emit signal for item id change
  emit dataChanged( itemIndex, itemIndex );
}

void QgsLayoutModel::updateItemLockStatus( QgsLayoutItem *item )
{
  if ( !item )
  {
    //nothing to do
    return;
  }

  //need to get QModelIndex of item
  QModelIndex itemIndex = indexForItem( item, LockStatus );
  if ( !itemIndex.isValid() )
  {
    return;
  }

  //emit signal for item lock status change
  emit dataChanged( itemIndex, itemIndex );
}

void QgsLayoutModel::updateItemVisibility( QgsLayoutItem *item )
{
  if ( !item )
  {
    //nothing to do
    return;
  }

  //need to get QModelIndex of item
  QModelIndex itemIndex = indexForItem( item, Visibility );
  if ( !itemIndex.isValid() )
  {
    return;
  }

  //emit signal for item visibility change
  emit dataChanged( itemIndex, itemIndex );
}

void QgsLayoutModel::updateItemSelectStatus( QgsLayoutItem *item )
{
  if ( !item )
  {
    //nothing to do
    return;
  }

  //need to get QModelIndex of item
  QModelIndex itemIndex = indexForItem( item, ItemId );
  if ( !itemIndex.isValid() )
  {
    return;
  }

  //emit signal for item visibility change
  emit dataChanged( itemIndex, itemIndex );
}

bool QgsLayoutModel::reorderItemUp( QgsLayoutItem *item )
{
  if ( !item )
  {
    return false;
  }

  if ( mItemsInScene.at( 0 ) == item )
  {
    //item is already topmost item present in scene, nothing to do
    return false;
  }

  //move item in z list
  QMutableListIterator<QgsLayoutItem *> it( mItemZList );
  if ( !it.findNext( item ) )
  {
    //can't find item in z list, nothing to do
    return false;
  }

  const QList< QGraphicsItem * > sceneItems = mLayout->items();

  it.remove();
  while ( it.hasPrevious() )
  {
    //search through item z list to find previous item which is present in the scene
    it.previous();
    if ( it.value() && sceneItems.contains( it.value() ) )
    {
      break;
    }
  }
  it.insert( item );

  //also move item in scene items z list and notify of model changes
  refreshAfterZOrderMove( item );
  return true;
}

bool QgsLayoutModel::reorderItemDown( QgsLayoutItem *item )
{
  if ( !item )
  {
    return false;
  }

  if ( mItemsInScene.last() == item )
  {
    //item is already lowest item present in scene, nothing to do
    return false;
  }

  //move item in z list
  QMutableListIterator<QgsLayoutItem *> it( mItemZList );
  if ( !it.findNext( item ) )
  {
    //can't find item in z list, nothing to do
    return false;
  }

  const QList< QGraphicsItem * > sceneItems = mLayout->items();
  it.remove();
  while ( it.hasNext() )
  {
    //search through item z list to find next item which is present in the scene
    //(deleted items still exist in the z list so that they can be restored to their correct stacking order,
    //but since they are not in the scene they should be ignored here)
    it.next();
    if ( it.value() && sceneItems.contains( it.value() ) )
    {
      break;
    }
  }
  it.insert( item );

  //also move item in scene items z list and notify of model changes
  refreshAfterZOrderMove( item );
  return true;
}

bool QgsLayoutModel::reorderItemToTop( QgsLayoutItem *item )
{
  if ( !item || !mItemsInScene.contains( item ) )
  {
    return false;
  }

  if ( mItemsInScene.at( 0 ) == item )
  {
    //item is already topmost item present in scene, nothing to do
    return false;
  }

  //move item in z list
  QMutableListIterator<QgsLayoutItem *> it( mItemZList );
  if ( it.findNext( item ) )
  {
    it.remove();
  }
  mItemZList.push_front( item );

  //also move item in scene items z list and notify of model changes
  refreshAfterZOrderMove( item );
  return true;
}

bool QgsLayoutModel::reorderItemToBottom( QgsLayoutItem *item )
{
  if ( !item || !mItemsInScene.contains( item ) )
  {
    return false;
  }

  if ( mItemsInScene.last() == item )
  {
    //item is already lowest item present in scene, nothing to do
    return false;
  }

  //move item in z list
  QMutableListIterator<QgsLayoutItem *> it( mItemZList );
  if ( it.findNext( item ) )
  {
    it.remove();
  }
  mItemZList.push_back( item );

  //also move item in scene items z list and notify of model changes
  refreshAfterZOrderMove( item );
  return true;
}

QgsLayoutItem *QgsLayoutModel::findItemAbove( QgsLayoutItem *item ) const
{
  //search item z list for selected item
  QListIterator<QgsLayoutItem *> it( mItemZList );
  it.toBack();
  if ( it.findPrevious( item ) )
  {
    //move position to before selected item
    while ( it.hasPrevious() )
    {
      //now find previous item, since list is sorted from lowest->highest items
      if ( it.hasPrevious() && !it.peekPrevious()->isGroupMember() )
      {
        return it.previous();
      }
      it.previous();
    }
  }
  return nullptr;
}

QgsLayoutItem *QgsLayoutModel::findItemBelow( QgsLayoutItem *item ) const
{
  //search item z list for selected item
  QListIterator<QgsLayoutItem *> it( mItemZList );
  if ( it.findNext( item ) )
  {
    //return next item (list is sorted from lowest->highest items)
    while ( it.hasNext() )
    {
      if ( !it.peekNext()->isGroupMember() )
      {
        return it.next();
      }
      it.next();
    }
  }
  return nullptr;
}

QList<QgsLayoutItem *> &QgsLayoutModel::zOrderList()
{
  return mItemZList;
}

///@endcond

Qt::ItemFlags QgsLayoutModel::flags( const QModelIndex &index ) const
{
  Qt::ItemFlags flags = QAbstractItemModel::flags( index );

  if ( !index.isValid() )
  {
    return flags | Qt::ItemIsDropEnabled;
  }

  // Top level row 0 is the reserved paper sentinel and carries no item, so it
  // gets neither checkboxes nor drag handles. Inside a group row 0 is an
  // ordinary member, hence the parent check.
  if ( index.row() == 0 && !index.parent().isValid() )
  {
    return flags | Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  }

  //a group is a drop target. QTreeView also tests this flag on the parent of a
  //row before it will offer an insertion point between that parent's children,
  //so without it items can neither be dropped onto a group nor restacked
  //inside one
  if ( qobject_cast<QgsLayoutItemGroup *>( itemFromIndex( index ) ) )
  {
    flags |= Qt::ItemIsDropEnabled;
  }

  switch ( index.column() )
  {
    case Visibility:
    case LockStatus:
      return flags | Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
    case ItemId:
      return flags | Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
    default:
      return flags | Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  }
}

QModelIndex QgsLayoutModel::indexForItem( QgsLayoutItem *item, const int column )
{
  if ( !item )
  {
    return QModelIndex();
  }

  QgsLayoutItemGroup *parentGroup = item->parentGroup();
  if ( !parentGroup )
  {
    const QList<QgsLayoutItem *> top = topLevelItemsInScene();
    int row = top.indexOf( item );
    if ( row < 0 )
      return QModelIndex();
    return index( row + 1, column ); // +1 for sentinel
  }

  const QList<QgsLayoutItem *> siblings = childItemsInScene( parentGroup );
  int row = siblings.indexOf( item );
  if ( row < 0 )
    return QModelIndex();
  QModelIndex parentIdx = indexForItem( parentGroup, 0 );
  return index( row, column, parentIdx );
}

///@cond PRIVATE
void QgsLayoutModel::setSelected( const QModelIndex &index )
{
  QgsLayoutItem *item = itemFromIndex( index );
  if ( !item )
  {
    return;
  }

  // find top level group this item is contained within, and mark the group as selected
  QgsLayoutItemGroup *group = item->parentGroup();
  while ( group && group->parentGroup() )
  {
    group = group->parentGroup();
  }

  // but the actual main selected item is the item itself (allows editing of item properties)
  mLayout->setSelectedItem( item );

  if ( group && group != item )
    group->setSelected( true );
}
///@endcond

//
// QgsLayoutProxyModel
//

QgsLayoutProxyModel::QgsLayoutProxyModel( QgsLayout *layout, QObject *parent )
  : QSortFilterProxyModel( parent )
  , mLayout( layout )
{
  if ( mLayout )
    setSourceModel( mLayout->itemsModel() );

  setDynamicSortFilter( true );
  setSortLocaleAware( true );
  sort( QgsLayoutModel::ItemId );
}

bool QgsLayoutProxyModel::lessThan( const QModelIndex &left, const QModelIndex &right ) const
{
  const QString leftText = sourceModel()->data( left, Qt::DisplayRole ).toString();
  const QString rightText = sourceModel()->data( right, Qt::DisplayRole ).toString();
  if ( leftText.isEmpty() )
    return true;
  if ( rightText.isEmpty() )
    return false;

  //sort by item id
  const QgsLayoutItem *item1 = itemFromSourceIndex( left );
  const QgsLayoutItem *item2 = itemFromSourceIndex( right );
  if ( !item1 )
    return false;

  if ( !item2 )
    return true;

  return QString::localeAwareCompare( item1->displayName(), item2->displayName() ) < 0;
}

QgsLayoutItem *QgsLayoutProxyModel::itemFromSourceIndex( const QModelIndex &sourceIndex ) const
{
  if ( !mLayout )
    return nullptr;

  //get column corresponding to an index from the source model
  QVariant itemAsVariant = sourceModel()->data( sourceIndex, Qt::UserRole + 1 );
  return qobject_cast<QgsLayoutItem *>( itemAsVariant.value<QObject *>() );
}

void QgsLayoutProxyModel::setAllowEmptyItem( bool allowEmpty )
{
  mAllowEmpty = allowEmpty;
  invalidateFilter();
}

bool QgsLayoutProxyModel::allowEmptyItem() const
{
  return mAllowEmpty;
}

void QgsLayoutProxyModel::setItemFlags( QgsLayoutItem::Flags flags )
{
  mItemFlags = flags;
  invalidateFilter();
}

QgsLayoutItem::Flags QgsLayoutProxyModel::itemFlags() const
{
  return mItemFlags;
}

void QgsLayoutProxyModel::setFilterType( QgsLayoutItemRegistry::ItemType filter )
{
  mItemTypeFilter = filter;
  invalidate();
}

void QgsLayoutProxyModel::setExceptedItemList( const QList< QgsLayoutItem *> &items )
{
  if ( mExceptedList == items )
    return;

  mExceptedList = items;
  invalidateFilter();
}

bool QgsLayoutProxyModel::filterAcceptsRow( int sourceRow, const QModelIndex &sourceParent ) const
{
  //get QgsComposerItem corresponding to row
  QModelIndex index = sourceModel()->index( sourceRow, 0, sourceParent );
  QgsLayoutItem *item = itemFromSourceIndex( index );

  if ( !item )
    return mAllowEmpty;

  // specific exceptions
  if ( mExceptedList.contains( item ) )
    return false;

  // filter by type
  if ( mItemTypeFilter != QgsLayoutItemRegistry::LayoutItem && item->type() != mItemTypeFilter )
    return false;

  if ( mItemFlags && !( item->itemFlags() & mItemFlags ) )
  {
    return false;
  }

  return true;
}
