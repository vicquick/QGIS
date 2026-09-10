/***************************************************************************
                         qgslayoutmodel.h
                         ----------------
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

#ifndef QGSLAYOUTMODEL_H
#define QGSLAYOUTMODEL_H

#include "qgis_core.h"
#include "qgis_sip.h"
#include "qgslayoutitemregistry.h"

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>

#include <memory>

class QgsLayout;
class QGraphicsItem;
class QgsLayoutItem;
class QgsLayoutItemGroup;

/**
 * \class QgsLayoutModel
 * \ingroup core
 *
 * \brief A model for items attached to a layout.
 *
 * The model also maintains the z-order for the layout, and must be notified whenever item stacking changes.
 *
 * Internally, QgsLayoutModel maintains two lists. One contains a complete list of all items for
 * the layout, ordered by their position within the z-order stack.
 *
 * The second list contains only items which are currently displayed in the layout's scene.
 * It is used as a cache of the last known stacking order, so that the model can compare the current
 * stacking of items in the layout to the last known state, and emit the corresponding signals
 * as required.
 *
 */

class CORE_EXPORT QgsLayoutModel : public QAbstractItemModel
{
    Q_OBJECT

  public:
    //! Columns returned by the model
    enum Columns
    {
      Visibility = 0, //!< Item visibility checkbox
      LockStatus,     //!< Item lock status checkbox
      ItemId,         //!< Item ID
    };

    /**
     * Constructor for a QgsLayoutModel attached to the specified \a layout.
     */
    explicit QgsLayoutModel( QgsLayout *layout, QObject *parent SIP_TRANSFERTHIS = nullptr );

    //reimplemented QAbstractItemModel methods
    QModelIndex index( int row, int column, const QModelIndex &parent = QModelIndex() ) const override;
    QModelIndex parent( const QModelIndex &index ) const override;
    int rowCount( const QModelIndex &parent = QModelIndex() ) const override;
    int columnCount( const QModelIndex &parent = QModelIndex() ) const override;
    QVariant data( const QModelIndex &index, int role ) const override;
    Qt::ItemFlags flags( const QModelIndex &index ) const override;
    bool setData( const QModelIndex &index, const QVariant &value, int role ) override;
    QVariant headerData( int section, Qt::Orientation orientation, int role = Qt::DisplayRole ) const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData( const QModelIndexList &indexes ) const override;
    bool dropMimeData( const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent ) override;
    bool removeRows( int row, int count, const QModelIndex &parent = QModelIndex() ) override;

///@cond PRIVATE
#ifndef SIP_RUN

    /**
     * Clears all items from z-order list and resets the model
     */
    void clear();

    /**
     * Returns the size of the z-order list.
     */
    int zOrderListSize() const;

    /**
     * Rebuilds the z-order list, based on the current stacking of items in the layout.
     * This method should be called after adding multiple items to the layout.
     */
    void rebuildZList();

    /**
     * Adds an \a item to the top of the layout z stack. The item must not already exist in the z-order list.
     * \see reorderItemToTop()
     */
    void addItemAtTop( QgsLayoutItem *item );

    /**
     * Removes an \a item from the z-order list.
     */
    void removeItem( QgsLayoutItem *item );

    /**
     * Moves an \a item up the z-order list.
     *
     * Returns TRUE if \a item was moved. Returns FALSE if \a item was not found
     * in z-order list or was already at the top of the z-order list.
     *
     * \see reorderItemDown()
     * \see reorderItemToTop()
     * \see reorderItemToBottom()
     */
    bool reorderItemUp( QgsLayoutItem *item );

    /**
     * Moves an \a item down the z-order list.
     *
     * Returns TRUE if \a item was moved. Returns FALSE if \a item was not found
     * in z-order list or was already at the bottom of the z-order list.
     *
     * \see reorderItemUp()
     * \see reorderItemToTop()
     * \see reorderItemToBottom()
     */
    bool reorderItemDown( QgsLayoutItem *item );

    /**
     * Moves an \a item to the top of the z-order list.
     *
     * Returns TRUE if \a item was moved. Returns FALSE if \a item was not found
     * in z-order list or was already at the top of the z-order list.
     *
     * \see reorderItemUp()
     * \see reorderItemDown()
     * \see reorderItemToBottom()
     */
    bool reorderItemToTop( QgsLayoutItem *item );

    /**
     * Moves an \a item to the bottom of the z-order list.
     *
     * Returns TRUE if \a item was moved. Returns FALSE if \a item was not found
     * in z-order list or was already at the bottom of the z-order list.
     *
     * \see reorderItemUp()
     * \see reorderItemDown()
     * \see reorderItemToTop()
     */
    bool reorderItemToBottom( QgsLayoutItem *item );

    /**
     * Finds the next layout item above an \a item, where \a item is
     * the item to search above.
     *
     * If no items were found, NULLPTR will be returned.
     *
     * \see findItemBelow()
     */
    QgsLayoutItem *findItemAbove( QgsLayoutItem *item ) const;

    /**
     * Finds the next layout item below an \a item, where \a item
     * is the item to search below.
     *
     * If no items were found, NULLPTR will be returned.

     * \see findItemAbove()
     */
    QgsLayoutItem *findItemBelow( QgsLayoutItem *item ) const;

    /**
     * Returns the item z-order list.
     */
    QList<QgsLayoutItem *> &zOrderList();

    /**
     * Marks an \a item as removed from the layout. This must be called whenever an item
     * is about to be removed from the layout.
     */
    void setItemRemoved( QgsLayoutItem *item );

#if 0

    /**
     * Restores an item to the composition. This must be called whenever an item removed
     * from the composition is restored to the composition.
     * \param item to mark as restored to the composition
     * \see setItemRemoved
     */
    void setItemRestored( QgsComposerItem *item );
#endif

    /**
     * Must be called when an \a item's display name is modified.
     *
     * \see updateItemLockStatus()
     * \see updateItemVisibility()
     * \see updateItemSelectStatus()
     */
    void updateItemDisplayName( QgsLayoutItem *item );

    /**
     * Must be called when an \a item's lock status changes.
     * \see updateItemDisplayName()
     * \see updateItemVisibility()
     * \see updateItemSelectStatus()
     */
    void updateItemLockStatus( QgsLayoutItem *item );

    /**
     * Must be called when an \a item's visibility changes.
     * \see updateItemDisplayName()
     * \see updateItemLockStatus()
     * \see updateItemSelectStatus()
     */
    void updateItemVisibility( QgsLayoutItem *item );

    /**
     * Must be called when an \a item's selection status changes.
     * \see updateItemDisplayName()
     * \see updateItemVisibility()
     * \see updateItemLockStatus()
     */
    void updateItemSelectStatus( QgsLayoutItem *item );
#endif
    ///@endcond

    /**
     * Returns the QgsLayoutItem corresponding to a QModelIndex \a index, if possible.
     * \see indexForItem()
     */
    QgsLayoutItem *itemFromIndex( const QModelIndex &index ) const;

    /**
     * Returns the QModelIndex corresponding to a QgsLayoutItem \a item and \a column, if possible.
     * \see itemFromIndex()
     */
    QModelIndex indexForItem( QgsLayoutItem *item, int column = 0 );

    /**
     * Re-emits modelReset around an externally-driven structural change
     * (e.g. grouping or ungrouping) so the tree picks up new
     * parentGroup() relationships. Cheap fallback for operations that
     * would otherwise need granular beginMoveRows / endMoveRows.
     */
    void emitModelReset();

  public slots:

///@cond PRIVATE
#ifndef SIP_RUN

    /**
     * Sets an item as the current selection from a QModelIndex \a index.
     */
    void setSelected( const QModelIndex &index );
#endif
    ///@endcond

  private:
    //! Maintains z-Order of items. Starts with item at position 1 (position 0 is always paper item)
    QList<QgsLayoutItem *> mItemZList;

    //! Cached list of items from mItemZList which are currently in the scene
    QList<QgsLayoutItem *> mItemsInScene;

    /**
     * Membership of mItemsInScene, for O(1) lookup.
     *
     * itemFromIndex() has to reject an index whose item the model has already
     * let go of, and it is called from data(), flags(), index(), parent(),
     * rowCount(), setData() and mimeData() - i.e. several times per painted
     * cell. Testing QList::contains() there would turn those O(1) paths into
     * O(N) and cost on the order of 20 * visibleRows * N pointer compares per
     * repaint. childItemsInScene() does the same test per group child.
     *
     * Kept in sync in exactly the two places that write mItemsInScene:
     * refreshItemsInScene() and rebuildSceneItemList().
     */
    QSet<QgsLayoutItem *> mItemsInSceneSet;

    //! Parent layout
    QgsLayout *mLayout = nullptr;

    //! Resyncs mItemsInSceneSet from mItemsInScene. Call after any write to the list.
    void refreshItemsInSceneSet();

    /**
     * Rebuilds the list of all layout items which are present in the layout. This is
     * called when the stacking of order changes or when items are removed/restored to the
     * layout. Unlike rebuildSceneItemList, this method clears the existing scene item
     * list and does not emit QAbstractItemModel signals. Accordingly, this method should
     * only be called when changes to the z-order list are known and QAbstractItemModel begin
     * signals have already been called.
     * \see rebuildSceneItemList()
     */
    void refreshItemsInScene();

    /**
     * Steps through the item z-order list and rebuilds the items in layout list,
     * emitting QAbstractItemModel signals as required.
     * \see refreshItemsInScene()
     */
    void rebuildSceneItemList();

    /**
     * Returns items from mItemsInScene that have no parent group.
     * Order preserves the existing global z-order slice.
     */
    QList<QgsLayoutItem *> topLevelItemsInScene() const;

    /**
     * Returns items from mItemsInScene whose parentGroup() is \a group.
     * Order preserves the existing global z-order slice — once
     * QgsLayoutItemGroup gains its own local z-stack this will defer
     * to that ordering.
     */
    QList<QgsLayoutItem *> childItemsInScene( class QgsLayoutItemGroup *group ) const;

    /**
     * Returns the items which topLevelItemsInScene() will expose once
     * refreshItemsInScene() has caught up with the current contents of the
     * z-order list.
     */
    QList<QgsLayoutItem *> prospectiveTopLevelItems() const;

    /**
     * Rebuilds the scene item cache after \a item has been moved within the
     * z-order list, emitting the row move which matches the item's position in
     * the tree the model exposes.
     */
    void refreshAfterZOrderMove( QgsLayoutItem *item );

    //! Where a restack takes an item, see reorderTopLevelItem() and reorderGroupMember()
    enum class ReorderDirection
    {
      Up,     //!< One step towards the top of the stack
      Down,   //!< One step towards the bottom of the stack
      Top,    //!< All the way to the top of the stack
      Bottom  //!< All the way to the bottom of the stack
    };

    /**
     * Moves the whole z-order run belonging to the top level \a item - the item
     * itself and, for a group, everything that group contains - one place in
     * \a direction past the neighbouring top level run.
     *
     * Returns TRUE if the run was moved.
     */
    bool reorderTopLevelItem( QgsLayoutItem *item, ReorderDirection direction );

    /**
     * Moves \a item in \a direction within the local z-stack of the \a group
     * holding it, rewrites that group's run in the z-order list to match, and
     * records the move on the undo stack.
     *
     * Returns TRUE if the item was moved.
     */
    bool reorderGroupMember( QgsLayoutItemGroup *group, QgsLayoutItem *item, ReorderDirection direction );

    friend class TestQgsLayoutModel;
    friend class TestQgsLayoutGui;
};


#ifndef SIP_RUN

/**
 * \class QgsLayoutModelFlattener
 * \ingroup core
 *
 * \brief Presents the rows of a hierarchical QgsLayoutModel as a single flat list, in
 * depth-first order.
 *
 * QgsLayoutModel is a tree: a grouped item's row lives one level below the model's
 * root, as a child of its group's row. A flat view such as a combo box only ever
 * enumerates the root level of whatever model it is given, so once QgsLayoutModel
 * gained groups, anything sitting downstream of it that still assumed a flat list
 * stopped reaching grouped items at all.
 *
 * This proxy sits directly on top of QgsLayoutModel, upstream of any further
 * filtering or sorting proxy, and re-exposes every row of the tree - top-level
 * items, group items and their members alike - as a single flat list at row depth
 * zero, so that a flat consumer can reach grouped items again.
 *
 * Structural changes from the source model always trigger a full reset here rather
 * than incremental row signals. Layout item counts are small, and getting a
 * beginMoveRows()/endMoveRows() pair wrong on a tree this shape has already crashed
 * this model twice - a full reset cannot be refused by Qt and cannot desync the
 * view from the model, so it is the only shape used here.
 *
 * \note Not available in Python bindings.
 */
class CORE_EXPORT QgsLayoutModelFlattener : public QAbstractProxyModel
{
    Q_OBJECT

  public:
    explicit QgsLayoutModelFlattener( QObject *parent = nullptr );

    void setSourceModel( QAbstractItemModel *sourceModel ) override;

    QModelIndex index( int row, int column, const QModelIndex &parent = QModelIndex() ) const override;
    QModelIndex parent( const QModelIndex &child ) const override;
    int rowCount( const QModelIndex &parent = QModelIndex() ) const override;
    int columnCount( const QModelIndex &parent = QModelIndex() ) const override;
    QModelIndex mapToSource( const QModelIndex &proxyIndex ) const override;
    QModelIndex mapFromSource( const QModelIndex &sourceIndex ) const override;

  private slots:

    //! Rebuilds mFlatList from a depth-first walk of the source model, as a full reset.
    void rebuild();

  private:
    //! Depth-first, column-0 walk of the source tree, one entry per row it exposes.
    QList<QPersistentModelIndex> mFlatList;
};

#endif

/**
 * \class QgsLayoutProxyModel
 * \ingroup core
 * \brief Allows for filtering a QgsLayoutModel by item type.
 */
class CORE_EXPORT QgsLayoutProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

  public:
    /**
     * Constructor for QgsLayoutProxyModelm, attached to the specified \a layout.
     */
    QgsLayoutProxyModel( QgsLayout *layout, QObject *parent SIP_TRANSFERTHIS = nullptr );

    /**
     * Returns the current item type filter, or QgsLayoutItemRegistry::LayoutItem if no
     * item type filter is set.
     * \see setFilterType()
     */
    QgsLayoutItemRegistry::ItemType filterType() const { return mItemTypeFilter; }

    /**
     * Sets the item type \a filter. Only matching item types will be shown.
     * Set \a filter to QgsLayoutItemRegistry::LayoutItem to show all
     * item types.
     * \see filterType()
     */
    void setFilterType( QgsLayoutItemRegistry::ItemType filter );

    /**
     * Sets a list of specific \a items to exclude from the model.
     * \see exceptedItemList()
     */
    void setExceptedItemList( const QList< QgsLayoutItem * > &items );

    /**
     * Returns the list of specific items excluded from the model.
     * \see setExceptedItemList()
     */
    QList< QgsLayoutItem * > exceptedItemList() const { return mExceptedList; }

    /**
     * Returns the QgsLayoutModel used in this proxy model.
     *
     * This is the layout's underlying item tree (see QgsLayout::itemsModel()), not
     * this proxy's immediate sourceModel() - an internal flattening layer sits
     * between the two so that grouped items remain reachable from a flat view. An
     * index from sourceLayerModel() is therefore in a different index space to one
     * from sourceModel()/mapToSource(); to get from one of the former to an index
     * usable on this proxy, use mapFromLayoutModel() rather than mapFromSource().
     */
    QgsLayoutModel *sourceLayerModel() const { return mLayout ? mLayout->itemsModel() : nullptr; }

    /**
     * Returns the QgsLayoutItem corresponding to an index from this proxy's
     * immediate sourceModel(), i.e. one obtained through mapToSource(). Note this
     * is not sourceLayerModel() - see its documentation for why.
     */
    QgsLayoutItem *itemFromSourceIndex( const QModelIndex &sourceIndex ) const;

    /**
     * Returns the model index, in this proxy model's own index space, corresponding
     * to \a layoutModelIndex, an index from sourceLayerModel() (the layout's
     * underlying item tree). Returns an invalid index if \a layoutModelIndex has no
     * corresponding, currently-accepted row in this proxy.
     *
     * Use this rather than mapFromSource() when starting from an index obtained via
     * sourceLayerModel() - see its documentation for why the two are not
     * interchangeable.
     */
    QModelIndex mapFromLayoutModel( const QModelIndex &layoutModelIndex ) const;

    /**
     * Returns the associated layout.
     * \since QGIS 3.8
     */
    QgsLayout *layout() { return mLayout; }

    /**
     * Sets whether an optional empty layout item is present in the model.
     * \see allowEmptyItem()
     * \since QGIS 3.8
     */
    void setAllowEmptyItem( bool allowEmpty );

    /**
     * Returns TRUE if the model includes the empty item choice.
     * \see setAllowEmptyItem()
     * \since QGIS 3.8
     */
    bool allowEmptyItem() const;

    /**
     * Sets layout item flags to use for filtering the available items.
     *
     * Set \a flags to NULLPTR to clear the flag based filtering.
     *
     * \see itemFlags()
     * \since QGIS 3.16
     */
    void setItemFlags( QgsLayoutItem::Flags flags );

    /**
     * Returns the layout item flags used for filtering the available items.
     *
     * Returns NULLPTR if no flag based filtering is occurring.
     *
     * \see setItemFlags()
     * \since QGIS 3.16
     */
    QgsLayoutItem::Flags itemFlags() const;

  protected:
    bool filterAcceptsRow( int sourceRow, const QModelIndex &sourceParent ) const override;
    bool lessThan( const QModelIndex &left, const QModelIndex &right ) const override;

  private:
    QgsLayout *mLayout = nullptr;
    QgsLayoutItemRegistry::ItemType mItemTypeFilter = QgsLayoutItemRegistry::LayoutItem;
    QList< QgsLayoutItem * > mExceptedList;
    bool mAllowEmpty = false;
    QgsLayoutItem::Flags mItemFlags = QgsLayoutItem::Flags();

    //! Flattens sourceLayerModel()'s tree; this proxy's actual sourceModel(). See sourceLayerModel().
    std::unique_ptr<QgsLayoutModelFlattener> mFlattener;
};


#endif //QGSLAYOUTMODEL_H
