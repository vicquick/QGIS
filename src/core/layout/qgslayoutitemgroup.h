/***************************************************************************
                              qgslayoutitemgroup.h
                             ---------------------
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

#ifndef QGSLAYOUTITEMGROUP_H
#define QGSLAYOUTITEMGROUP_H

#include "qgis_core.h"
#include "qgslayoutitem.h"

/**
 * \ingroup core
 * \brief A container for grouping several QgsLayoutItems.
 */
class CORE_EXPORT QgsLayoutItemGroup : public QgsLayoutItem
{
    Q_OBJECT

  public:
    /**
     * Constructor for QgsLayoutItemGroup, belonging to the specified \a layout.
     */
    explicit QgsLayoutItemGroup( QgsLayout *layout );
    ~QgsLayoutItemGroup() override;

    void cleanup() override;

    int type() const override;
    QString displayName() const override;

    /**
     * Returns a new group item for the specified \a layout.
     *
     * The caller takes responsibility for deleting the returned object.
     */
    static QgsLayoutItemGroup *create( QgsLayout *layout ) SIP_FACTORY;

    /**
     * Adds an \a item to the group. Ownership of the item
     * is transferred to the group.
    */
    void addItem( QgsLayoutItem *item SIP_TRANSFER );

    /**
     * Inserts an \a item into the group at position \a index of the group's
     * local z-stack (0 = topmost). Ownership of the item is transferred to
     * the group.
     *
     * An \a index outside the current range appends the item to the bottom
     * of the stack. If \a item is already a member of the group it is moved
     * to \a index instead of being added a second time.
     *
     * \see addItem()
     */
    void insertItem( QgsLayoutItem *item SIP_TRANSFER, int index );

    /**
     * Removes a single \a item from the group (but does not delete it).
     * The item remains in the scene but is no longer grouped.
     *
     * Returns TRUE if \a item was a member of the group.
     *
     * \see removeItems()
     */
    bool removeItem( QgsLayoutItem *item );

    /**
     * Removes all items from the group (but does not delete them).
     * Items remain in the scene but are no longer grouped together
     */
    void removeItems();

    /**
     * Returns a list of items contained by the group, in local z-order
     * (index 0 = topmost within the group, last index = bottommost).
     */
    QList<QgsLayoutItem *> items() const;

    /**
     * Moves an \a item one step toward the top of the group's local z-stack.
     * Returns TRUE if \a item was moved.
     */
    bool reorderItemUp( QgsLayoutItem *item );

    /**
     * Moves an \a item one step toward the bottom of the group's local z-stack.
     * Returns TRUE if \a item was moved.
     */
    bool reorderItemDown( QgsLayoutItem *item );

    /**
     * Moves an \a item to the top of the group's local z-stack.
     * Returns TRUE if \a item was moved.
     */
    bool reorderItemToTop( QgsLayoutItem *item );

    /**
     * Moves an \a item to the bottom of the group's local z-stack.
     * Returns TRUE if \a item was moved.
     */
    bool reorderItemToBottom( QgsLayoutItem *item );

    //overridden to also hide grouped items
    void setVisibility( bool visible ) override;

    //overridden to move child items
    void attemptMove( const QgsLayoutPoint &point, bool useReferencePoint = true, bool includesFrame = false, int page = -1 ) override;
    void attemptResize( const QgsLayoutSize &size, bool includesFrame = false ) override;

    void paint( QPainter *painter, const QStyleOptionGraphicsItem *itemStyle, QWidget *pWidget ) override;

    void finalizeRestoreFromXml() override;
    ExportLayerBehavior exportLayerBehavior() const override;

    QRectF rectWithFrame() const override;

  protected:
    void draw( QgsLayoutItemRenderContext &context ) override;
    bool writePropertiesToElement( QDomElement &parentElement, QDomDocument &document, const QgsReadWriteContext &context ) const override;
    bool readPropertiesFromElement( const QDomElement &itemElement, const QDomDocument &document, const QgsReadWriteContext &context ) override;

  private slots:
    void updateBoundingRect();

  private:
    QList< QString > mItemUuids;
    QList< QPointer< QgsLayoutItem >> mItems;
    QRectF mRectWithFrame;
};

#endif //QGSLAYOUTITEMGROUP_H
