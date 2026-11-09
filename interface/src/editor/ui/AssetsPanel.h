//
//  AssetsPanel.h
//  interface/src/editor/ui
//
//  The Assets dock (NATIVE-EDITOR.md §7.4): the user's My Files as Unity shows a project, a folder tree beside a
//  grid of the folder's contents, with search, thumbnails, uploads by drop, the folder operations, and drags out
//  that carry a file's stable link.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AssetsPanel_h
#define hifi_AssetsPanel_h

#include <functional>

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QDockWidget>

#include <SettingHandle.h>

#include "UserFiles.h"

class QAbstractItemDelegate;
class QComboBox;
class QHBoxLayout;
class QHeaderView;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListView;
class QMimeData;
class QStandardItem;
class QToolButton;
class QTreeView;
class AssetDetailsDelegate;
class AssetGridView;
class AssetThumbnails;
class EditorUpload;

// The items of the folder tree and of the contents grid. A drag carries the files' stable links as text/uri-list,
// which every URL and picture field takes, and the panel's own description of what is dragged (ASSET_MIME_TYPE:
// a JSON array of {id, path, name, kind, url}), by which a drop on a folder is a move. Files from this computer
// dropped on a folder are uploads.
class AssetItemModel : public QStandardItemModel {
    Q_OBJECT
public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        NameRole,       // the name as listed: what a rename is measured against
        IdRole,
        UrlRole,
        PathUrlRole,
        FolderRole,
        KindRole,
        PrivateRole,
        PictureRole,    // true once the item shows the picture itself rather than a type icon
        SizeRole,       // bytes (qint64)
        UpdatedRole,    // QDateTime, invalid when unknown
        FileCountRole   // a folder's files at any depth
    };
    static const char* ASSET_MIME_TYPE;

    explicit AssetItemModel(QObject* parent = nullptr);
    // the folder a drop on no item goes into; empty: such drops are refused
    void setDropFolder(const QString& folderPath) { _dropFolder = folderPath; }

    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction | Qt::MoveAction; }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction | Qt::MoveAction; }

signals:
    // something landed on a folder: the panel moves or uploads it
    void dropped(const QMimeData* data, const QString& folderPath);

private:
    QString _dropFolder;
};

class AssetsPanel : public QDockWidget {
    Q_OBJECT
public:
    // how the folder's contents are shown: icons with the name under each, or rows with the name and the details
    enum class View { Icons, Details };
    // the Details view's columns, which are also what the contents can be sorted by
    enum Column { NameColumn, KindColumn, DateColumn, SizeColumn, IdColumn, ColumnCount };

    explicit AssetsPanel(QWidget* parent = nullptr);

    View view() const { return _view; }
    void setView(View view);
    // the order of the contents, in both views; folders come first whatever the order
    void setSort(Column column, bool descending);
    // build mode on: the listing is fetched, and again after every change; off: nothing is fetched
    void setActive(bool active);
    // a note at the foot, in normal play (Build mode has its status bar); empty hides it
    void setNote(const QString& text);
    // where the editor's own keys go when pressed in the panel (true: taken)
    void setKeyFallback(std::function<bool(QKeyEvent*)> fallback) { _keyFallback = std::move(fallback); }
    UserFiles* files() const { return _files; }
    QString folder() const { return _folder; }
    void setFolder(const QString& folderPath);
    // Upload… without its file dialog, for a scripted test
    void uploadFilesForTest(const QStringList& localPaths) { uploadFiles(localPaths, _folder); }

signals:
    // a note for the status bar: a link copied, an upload's progress, why something could not be done
    void status(const QString& text, int clearAfterMs);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildTree();
    void rebuildGrid();
    void sortFiles(QVector<const UserFile*>& files) const;
    void rebuildCrumbs();
    void updateQuota();
    void updateBanner();
    void placeEmptyNote();
    void requestThumbnails();
    QIcon typeIcon(const UserFile& file) const;
    QIcon pictureIcon(const QPixmap& picture, bool isPrivate) const;
    // a double-click or Open in Code: a script in the Code panel, anything else in the browser
    void openFile(const UserFile& file);
    bool opensInCode(const UserFile& file) const;
    void fillItem(QStandardItem* item, const UserFile& file) const;
    void onListed();
    void onDropped(const QMimeData* data, const QString& folderPath);
    void uploadFiles(const QStringList& localPaths, const QString& folderPath);
    // fromComputer: a file from this computer (not a duplicate), whose status carries the rights line
    void trackUpload(EditorUpload* upload, bool fromComputer);
    void duplicateFiles(const QList<const UserFile*>& files);
    void chooseUploads();
    void showGridMenu(const QPoint& globalPos);
    void showTreeMenu(const QPoint& globalPos);
    QList<const UserFile*> selectedFiles() const;
    const UserFile* treeFolderAt(const QModelIndex& index) const;
    void newFolder(const QString& inFolder);
    void deleteFiles(const QList<const UserFile*>& files);
    void renameFromItem(QStandardItem* item);
    void copyLinks(const QList<const UserFile*>& files, bool pathLinks);
    void copyIds(const QList<const UserFile*>& files);
    void openInBrowser(const UserFile& file);
    bool searching() const;

    UserFiles* _files { nullptr };
    AssetThumbnails* _thumbnails { nullptr };
    AssetItemModel* _treeModel { nullptr };
    AssetItemModel* _gridModel { nullptr };
    QTreeView* _tree { nullptr };
    AssetGridView* _grid { nullptr };
    QHeaderView* _header { nullptr };               // over the Details view: the columns, clicked to sort
    QStandardItemModel* _headerModel { nullptr };   // the header's columns
    QLineEdit* _search { nullptr };
    QComboBox* _kind { nullptr };
    QLabel* _banner { nullptr };
    QLabel* _rightsLine { nullptr };   // under the files (ui/RightsLine.h)
    QLabel* _quota { nullptr };
    QLabel* _note { nullptr };
    QLabel* _empty { nullptr };     // a note over an empty grid
    QWidget* _crumbs { nullptr };
    QHBoxLayout* _crumbLayout { nullptr };
    QToolButton* _viewButtons[2] { nullptr, nullptr };   // by View
    QAbstractItemDelegate* _iconDelegate { nullptr };    // the view's own, for the Icons view
    AssetDetailsDelegate* _detailsDelegate { nullptr };

    View _view { View::Icons };
    Setting::Handle<int> _viewSetting { "editor/assetsView", (int)View::Icons };
    Column _sortColumn { NameColumn };
    bool _sortDescending { false };
    Setting::Handle<int> _sortSetting { "editor/assetsSort", (int)NameColumn };
    Setting::Handle<bool> _sortDescendingSetting { "editor/assetsSortDescending", false };
    QString _folder { "/" };
    QSet<QString> _expanded { "/" };              // the tree's open folders, kept across listings
    QHash<QString, QStandardItem*> _treeItems;    // by folder path
    QHash<QString, QStandardItem*> _gridItems;    // by path
    QHash<QString, QStandardItem*> _gridById;     // pictures, by id
    QString _editAfterListing;                    // a folder just made: selected and renamed in place once listed
    QTimer _searchTimer;
    QTimer _thumbnailTimer;
    QTimer _refreshTimer;
    std::function<bool(QKeyEvent*)> _keyFallback;
    bool _active { false };
    bool _updating { false };
    int _uploadsActive { 0 };
    mutable QHash<QString, QIcon> _typeIcons;
};

#endif // hifi_AssetsPanel_h
