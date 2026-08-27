/***************************************************************************
                          qgslayoutitemgroupundocommand.h
                          -------------------------------
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

#ifndef QGSLAYOUTITEMGROUPUNDOCOMMAND_H
#define QGSLAYOUTITEMGROUPUNDOCOMMAND_H

#include "qgis_core.h"
#include "qgslayoutitem.h"

#include <QUndoCommand>

#define SIP_NO_FILE

///@cond PRIVATE

/**
 * \ingroup core
 * \brief A layout undo command class for grouping / ungrouping layout items.
 */
class CORE_EXPORT QgsLayoutItemGroupUndoCommand : public QObject, public QUndoCommand
{
    Q_OBJECT

  public:
    //! Command kind, and state
    enum State
    {
      Grouped = 0,
      Ungrouped
    };

    /**
     * Create a group or ungroup command
     *
     * \param s command kind (\see State)
     * \param item the group item being created or ungrouped
     * \param c the composition including this group
     * \param text command label
     * \param parent parent command, if any
     *
     */
    QgsLayoutItemGroupUndoCommand( State s, QgsLayoutItemGroup *group, QgsLayout *layout, const QString &text, QUndoCommand *parent = nullptr );

    void redo() override;
    void undo() override;

  private:
    QString mGroupUuid;
    QSet<QString> mItemUuids;
    QgsLayout *mLayout = nullptr;
    State mState;
    //! Flag to prevent execution when the command is pushed to the QUndoStack
    bool mFirstRun = true;

    //changes between added / removed state
    void switchState();
};

/**
 * \ingroup core
 * \brief A layout undo command class for moving layout items between groups.
 *
 * The hierarchy is recorded directly rather than replayed from item XML:
 * QgsLayoutItemGroup::finalizeRestoreFromXml() applies a restored member list
 * additively, so restoring an item's XML can put it back into a group but can
 * never take it out of one again.
 *
 * Only group membership is restored. The layout's stacking order is left to
 * the item state commands which QgsLayout::updateZValues() pushes alongside
 * this one.
 */
class CORE_EXPORT QgsLayoutItemReparentUndoCommand : public QUndoCommand
{
  public:
    //! An item's place in the layout hierarchy
    struct Placement
    {
        //! UUID of the item being placed
        QString itemUuid;

        //! UUID of the group holding the item, empty when the item sits at the top level
        QString groupUuid;

        //! Position of the item in the group's local z-stack, -1 for the bottom of the stack
        int index = -1;
    };

    /**
     * Creates a command which moves items from the placements in \a before to the
     * placements in \a after.
     *
     * The move is assumed to have been applied to \a layout already, so the first
     * redo() does nothing.
     *
     * \param layout the layout holding the items
     * \param before where the items sat before the move
     * \param after where the items sit after the move
     * \param text command label
     * \param parent parent command, if any
     */
    QgsLayoutItemReparentUndoCommand( QgsLayout *layout, const QList<Placement> &before, const QList<Placement> &after, const QString &text, QUndoCommand *parent = nullptr );

    void redo() override;
    void undo() override;

  private:
    QgsLayout *mLayout = nullptr;
    QList<Placement> mBefore;
    QList<Placement> mAfter;
    //! Flag to prevent execution when the command is pushed to the QUndoStack
    bool mFirstRun = true;

    //! Moves every item named in \a state to the group and local z-stack position it records
    void applyState( const QList<Placement> &state );
};
///@endcond

#endif // QGSLAYOUTITEMGROUPUNDOCOMMAND_H
