/***************************************************************************
                          qgslayoutitemgroupundocommand.cpp
                          ---------------------------
    begin                : 2016-06-09
    copyright            : (C) 2016 by Sandro Santilli
    email                : strk at kbt dot io
***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgslayoutitemgroupundocommand.h"

#include "qgslayout.h"
#include "qgslayoutitemgroup.h"
#include "qgslayoutmodel.h"
#include "qgsproject.h"

#include <algorithm>

#include "moc_qgslayoutitemgroupundocommand.cpp"

///@cond PRIVATE
QgsLayoutItemGroupUndoCommand::QgsLayoutItemGroupUndoCommand( State s, QgsLayoutItemGroup *group, QgsLayout *layout, const QString &text, QUndoCommand *parent )
  : QUndoCommand( text, parent )
  , mGroupUuid( group->uuid() )
  , mLayout( layout )
  , mState( s )
{
  const QList< QgsLayoutItem * > items = group->items();
  for ( QgsLayoutItem *i : items )
  {
    mItemUuids.insert( i->uuid() );
  }
}

void QgsLayoutItemGroupUndoCommand::redo()
{
  if ( mFirstRun )
  {
    mFirstRun = false;
    return;
  }
  switchState();
}

void QgsLayoutItemGroupUndoCommand::undo()
{
  if ( mFirstRun )
  {
    mFirstRun = false;
    return;
  }
  switchState();
}

void QgsLayoutItemGroupUndoCommand::switchState()
{
  if ( mState == Grouped )
  {
    // ungroup
    QgsLayoutItemGroup *group = dynamic_cast< QgsLayoutItemGroup * >( mLayout->itemByUuid( mGroupUuid ) );
    Q_ASSERT_X( group, "QgsLayoutItemGroupUndoCommand::switchState", "Could not find group" );
    group->removeItems();
    mLayout->removeLayoutItemPrivate( group );
    mState = Ungrouped;
  }
  else //Ungrouped
  {
    // find group by uuid...
    QgsLayoutItemGroup *group = dynamic_cast< QgsLayoutItemGroup * >( mLayout->itemByUuid( mGroupUuid ) );
    if ( !group )
    {
      group = new QgsLayoutItemGroup( mLayout );
      mLayout->addLayoutItemPrivate( group );
    }

    for ( const QString &childUuid : std::as_const( mItemUuids ) )
    {
      QgsLayoutItem *childItem = mLayout->itemByUuid( childUuid );
      group->addItem( childItem );
    }

    mState = Grouped;
  }
  mLayout->project()->setDirty( true );
}


//
// QgsLayoutItemReparentUndoCommand
//

QgsLayoutItemReparentUndoCommand::QgsLayoutItemReparentUndoCommand( QgsLayout *layout, const QList<Placement> &before, const QList<Placement> &after, const QString &text, QUndoCommand *parent )
  : QUndoCommand( text, parent )
  , mLayout( layout )
  , mBefore( before )
  , mAfter( after )
{
}

void QgsLayoutItemReparentUndoCommand::redo()
{
  if ( mFirstRun )
  {
    mFirstRun = false;
    return;
  }
  applyState( mAfter );
}

void QgsLayoutItemReparentUndoCommand::undo()
{
  if ( mFirstRun )
  {
    mFirstRun = false;
    return;
  }
  applyState( mBefore );
}

void QgsLayoutItemReparentUndoCommand::applyState( const QList<Placement> &state )
{
  //detach every item first: two items can swap groups, and an index only means
  //what it meant when it was recorded once the other moved items are out of the way
  QList<QgsLayoutItem *> items;
  items.reserve( state.size() );
  for ( const Placement &placement : state )
  {
    QgsLayoutItem *item = mLayout->itemByUuid( placement.itemUuid );
    items << item;
    if ( !item )
      continue;

    if ( QgsLayoutItemGroup *currentGroup = item->parentGroup() )
      currentGroup->removeItem( item );
  }

  //...then fill each group from the top of its local z-stack downwards, so that
  //an index recorded against the finished stack lands where it was recorded
  QList<int> order;
  order.reserve( state.size() );
  const int count = static_cast< int >( state.size() );
  for ( int i = 0; i < count; ++i )
    order << i;
  std::stable_sort( order.begin(), order.end(), [&state]( int a, int b ) { return state.at( a ).index < state.at( b ).index; } );

  for ( const int i : std::as_const( order ) )
  {
    QgsLayoutItem *item = items.at( i );
    const Placement &placement = state.at( i );
    if ( !item || placement.groupUuid.isEmpty() )
      continue;

    if ( QgsLayoutItemGroup *group = qobject_cast<QgsLayoutItemGroup *>( mLayout->itemByUuid( placement.groupUuid ) ) )
      group->insertItem( item, placement.index );
  }

  //index(), parent() and rowCount() are all derived live from parentGroup(), so
  //the tree has to be told the whole hierarchy moved under it
  mLayout->itemsModel()->emitModelReset();
  mLayout->project()->setDirty( true );
}
///@endcond
