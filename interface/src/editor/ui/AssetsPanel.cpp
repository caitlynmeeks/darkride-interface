//
//  AssetsPanel.cpp
//  interface/src/editor/ui
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssetsPanel.h"

#include <algorithm>
#include <functional>

#include <QtCore/QCollator>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeData>
#include <QtCore/QMimeDatabase>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QFontDatabase>
#include <QtGui/QPainterPath>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

#include <AccountManager.h>
#include <DependencyManager.h>
#include <NetworkAccessManager.h>

#include "../../InterfaceLogging.h"
#include "../../ui/Popup.h"
#include "../../ui/RightsLine.h"
#include "../EditorCreate.h"
#include "AssetThumbnails.h"
#include "../../ui/DockTitleBar.h"
#include "../../ui/CodeEditor.h"

const char* AssetItemModel::ASSET_MIME_TYPE = "application/x-darkride-asset";

namespace {
    const int ICON_PIXELS = 64;
    const int STATUS_MS = 6000;
    const int MAX_SEARCH_RESULTS = 2000;
    const QString ROOT_LABEL = "My Files";

    QString kindText(UserFile::Kind kind) {
        return UserFile::kindName(kind);
    }

    QColor kindColor(UserFile::Kind kind) {
        switch (kind) {
            case UserFile::Kind::Picture: return QColor("#4f8fc9");
            case UserFile::Kind::Model: return QColor("#c98a4f");
            case UserFile::Kind::Avatar: return QColor("#a56fc9");
            case UserFile::Kind::Sound: return QColor("#5fb37f");
            case UserFile::Kind::Script: return QColor("#c9b84f");
            default: return QColor("#7d7d7d");
        }
    }

    void paintLock(QPainter& painter, const QRectF& area) {
        // a small padlock on a dark disc, in the corner: the file is private
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 20, 20, 230));
        painter.drawEllipse(area);
        QRectF body(area.left() + area.width() * 0.28, area.top() + area.height() * 0.48, area.width() * 0.44, area.height() * 0.32);
        painter.setBrush(QColor("#e8c860"));
        painter.drawRoundedRect(body, 1.5, 1.5);
        QPen pen(QColor("#e8c860"), area.width() * 0.09);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        QRectF shackle(area.left() + area.width() * 0.34, area.top() + area.height() * 0.22, area.width() * 0.32, area.height() * 0.36);
        painter.drawArc(shackle, 0, 180 * 16);
    }

    // the icons of files that have no picture of their own: a folder, or a page with the kind's colour and the
    // extension on it
    QPixmap paintTypeIcon(UserFile::Kind kind, const QString& suffix, bool isPrivate, int pixels) {
        QPixmap pixmap(pixels, pixels);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal s = pixels / 64.0;
        if (kind == UserFile::Kind::Folder) {
            QPainterPath tab;
            tab.addRoundedRect(QRectF(8 * s, 14 * s, 22 * s, 10 * s), 2 * s, 2 * s);
            QPainterPath body;
            body.addRoundedRect(QRectF(8 * s, 20 * s, 48 * s, 32 * s), 3 * s, 3 * s);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#7f97ab"));
            painter.drawPath(tab);
            painter.setBrush(QColor("#9fb6c9"));
            painter.drawPath(body);
            painter.setBrush(QColor(255, 255, 255, 28));
            painter.drawRect(QRectF(8 * s, 20 * s, 48 * s, 3 * s));
        } else {
            // a page with a folded corner
            QPainterPath page;
            page.moveTo(14 * s, 6 * s);
            page.lineTo(40 * s, 6 * s);
            page.lineTo(52 * s, 18 * s);
            page.lineTo(52 * s, 58 * s);
            page.lineTo(14 * s, 58 * s);
            page.closeSubpath();
            painter.setPen(QPen(QColor("#262626"), 1.5 * s));
            painter.setBrush(QColor("#555555"));
            painter.drawPath(page);
            QPainterPath fold;
            fold.moveTo(40 * s, 6 * s);
            fold.lineTo(40 * s, 18 * s);
            fold.lineTo(52 * s, 18 * s);
            fold.closeSubpath();
            painter.setBrush(QColor("#3a3a3a"));
            painter.drawPath(fold);
            // the kind's band with the extension on it
            QRectF band(14 * s, 34 * s, 38 * s, 16 * s);
            painter.setPen(Qt::NoPen);
            painter.setBrush(kindColor(kind));
            painter.drawRect(band);
            QFont font = painter.font();
            font.setPixelSize((int)(suffix.length() > 3 ? 8.5 * s : 10 * s));
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(QColor("#ffffff"));
            painter.drawText(band, Qt::AlignCenter, suffix.left(5).toUpper());
        }
        if (isPrivate) {
            paintLock(painter, QRectF(40 * s, 40 * s, 20 * s, 20 * s));
        }
        painter.end();
        return pixmap;
    }

    // the view buttons' pictures: four squares for Icons, three lined rows for Details
    QIcon viewIcon(bool details) {
        QIcon icon;
        for (int pixels : { 16, 32 }) {
            QPixmap pixmap(pixels, pixels);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#c8c8c8"));
            const qreal s = pixels / 16.0;
            if (details) {
                for (int row = 0; row < 3; ++row) {
                    qreal y = (3 + row * 4) * s;
                    painter.drawRect(QRectF(2 * s, y, 3 * s, 2.5 * s));
                    painter.drawRect(QRectF(6.5 * s, y, 7.5 * s, 2.5 * s));
                }
            } else {
                for (int row = 0; row < 2; ++row) {
                    for (int column = 0; column < 2; ++column) {
                        painter.drawRoundedRect(QRectF((2.5 + column * 6) * s, (2.5 + row * 6) * s, 5 * s, 5 * s), s, s);
                    }
                }
            }
            painter.end();
            icon.addPixmap(pixmap);
        }
        return icon;
    }
}

// --- AssetDetailsDelegate -----------------------------------------------------------------------

// The Details view's rows, laid out by the header's columns: in the Name column a medium icon, the name in full
// and under it what else is known (a folder's file count, Private, the folder while searching); then the kind,
// the date, the size and the id, each in its column.
class AssetDetailsDelegate : public QStyledItemDelegate {
public:
    static const int ROW_HEIGHT = 56;
    static const int ICON_PIXELS = 48;

    explicit AssetDetailsDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
        _idFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        _idFont.setPixelSize(11);
    }
    void setHeader(QHeaderView* header) { _header = header; }
    // while a search is shown, a row says which folder its file is in
    void setShowsFolder(bool shows) { _showsFolder = shows; }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(240, ROW_HEIGHT);   // the view widens rows to itself
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        // the row's background (hover, selection) as the style sheet has it; the rest is drawn here
        opt.text.clear();
        opt.icon = QIcon();
        opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

        bool selected = opt.state & QStyle::State_Selected;
        QColor textColor(selected ? "#ffffff" : "#d2d2d2");
        QColor dimColor(selected ? "#d8e4f0" : "#8f8f8f");
        QFontMetrics metrics(opt.font);
        bool folder = index.data(AssetItemModel::FolderRole).toBool();
        painter->save();

        // the Name column: the icon, the name, and the line under it
        Layout layout = layoutOf(opt, index);
        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        icon.paint(painter, layout.icon, Qt::AlignCenter);
        painter->setFont(opt.font);
        painter->setPen(textColor);
        painter->drawText(layout.name, Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, layout.name.width()));
        if (!layout.about.isEmpty()) {
            painter->setPen(dimColor);
            painter->drawText(layout.about, Qt::AlignLeft | Qt::AlignVCenter,
                              metrics.elidedText(aboutText(index), Qt::ElideRight, layout.about.width()));
        }

        // the other columns
        for (int column = AssetsPanel::KindColumn; column < AssetsPanel::ColumnCount; ++column) {
            QRect cell = cellRect(opt.rect, column).adjusted(6, 0, -6, 0);
            if (cell.width() <= 0) {
                continue;
            }
            QString text;
            Qt::Alignment align = Qt::AlignLeft;
            QFont font = opt.font;
            switch (column) {
                case AssetsPanel::KindColumn:
                    text = UserFile::kindName((UserFile::Kind)index.data(AssetItemModel::KindRole).toInt());
                    break;
                case AssetsPanel::DateColumn: {
                    QDateTime updated = index.data(AssetItemModel::UpdatedRole).toDateTime();
                    text = updated.isValid() ? updated.toString("d MMM yyyy HH:mm") : QString();
                    break;
                }
                case AssetsPanel::SizeColumn:
                    text = folder ? QString() : UserFile::sizeText(index.data(AssetItemModel::SizeRole).toLongLong());
                    align = Qt::AlignRight;
                    break;
                case AssetsPanel::IdColumn:
                    text = index.data(AssetItemModel::IdRole).toString();
                    font = _idFont;
                    break;
                default:
                    break;
            }
            painter->setFont(font);
            painter->setPen(dimColor);
            painter->drawText(cell, align | Qt::AlignVCenter, QFontMetrics(font).elidedText(text, Qt::ElideRight, cell.width()));
        }
        painter->restore();
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        // the name's line, out to the Name column's right edge
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        Layout layout = layoutOf(opt, index);
        editor->setGeometry(layout.name.adjusted(-3, -3, 0, 3));
    }

private:
    struct Layout {
        QRect icon;
        QRect name;    // the first line, right of the icon to the column's right edge
        QRect about;   // the second line; empty when there is nothing to say
    };

    // a column's part of a row, as the header has it; the whole row when there is no header
    QRect cellRect(const QRect& row, int column) const {
        if (!_header) {
            return column == AssetsPanel::NameColumn ? row : QRect();
        }
        return QRect(row.left() + _header->sectionViewportPosition(column), row.top(), _header->sectionSize(column), row.height());
    }

    QString aboutText(const QModelIndex& index) const {
        QStringList parts;
        if (index.data(AssetItemModel::FolderRole).toBool()) {
            int count = index.data(AssetItemModel::FileCountRole).toInt();
            parts << (count == 0 ? QStringLiteral("empty") : count == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(count));
        }
        if (index.data(AssetItemModel::PrivateRole).toBool()) {
            parts << QStringLiteral("Private");
        }
        if (_showsFolder) {
            parts << "in " + UserFile::folderOf(index.data(AssetItemModel::PathRole).toString());
        }
        return parts.join(QStringLiteral(" · "));
    }

    Layout layoutOf(const QStyleOptionViewItem& opt, const QModelIndex& index) const {
        Layout layout;
        QRect cell = cellRect(opt.rect, AssetsPanel::NameColumn).adjusted(6, 0, -8, 0);
        layout.icon = QRect(cell.left(), cell.top() + (cell.height() - ICON_PIXELS) / 2, ICON_PIXELS, ICON_PIXELS);
        int lineHeight = QFontMetrics(opt.font).height();
        int textLeft = layout.icon.right() + 11;
        int textWidth = qMax(0, cell.right() - textLeft + 1);
        if (aboutText(index).isEmpty()) {
            layout.name = QRect(textLeft, cell.top() + (cell.height() - lineHeight) / 2, textWidth, lineHeight);
        } else {
            int textTop = cell.top() + (cell.height() - 2 * lineHeight - 2) / 2;
            layout.name = QRect(textLeft, textTop, textWidth, lineHeight);
            layout.about = QRect(textLeft, textTop + lineHeight + 2, textWidth, lineHeight);
        }
        return layout;
    }

    QHeaderView* _header { nullptr };
    QFont _idFont;
    bool _showsFolder { false };
};

// --- AssetGridView ------------------------------------------------------------------------------

// The contents view: a QListView whose press on an item is the start of a drag and nothing else (never a rubber
// band from an item, whatever state the view's own handling is left in after a drag), and which carries the
// Details view's header above its rows the way a tree view does.
class AssetGridView : public QListView {
public:
    explicit AssetGridView(QWidget* parent = nullptr) : QListView(parent) {}

    void setHeader(QHeaderView* header) {
        _header = header;
        updateGeometries();
    }
    void setHeaderShown(bool shown) {
        if (_header) {
            _header->setVisible(shown);
            updateGeometries();
        }
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        _pressedOnItem = event->button() == Qt::LeftButton && indexAt(event->pos()).isValid();
        _pressPos = event->pos();
        _dragDone = false;
        QListView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (_dragDone) {
            // the drag has ended; the release may not have reached us yet (macOS keeps it), so nothing until it does
            return;
        }
        if (_pressedOnItem && (event->buttons() & Qt::LeftButton)) {
            if ((event->pos() - _pressPos).manhattanLength() < QApplication::startDragDistance()) {
                return;
            }
            _pressedOnItem = false;
            _dragDone = true;
            startDrag(model()->supportedDragActions());
            setState(NoState);
            return;
        }
        QListView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        _pressedOnItem = false;
        _dragDone = false;
        QListView::mouseReleaseEvent(event);
    }

    void startDrag(Qt::DropActions supportedActions) override {
        QElapsedTimer timer;
        timer.start();
        int count = selectedIndexes().size();
        QListView::startDrag(supportedActions);
        qCDebug(interfaceapp) << "Assets: dragged" << count << "items for" << timer.elapsed() << "ms";
    }

    void keyboardSearch(const QString&) override {}   // the search box searches; letters go to the editor's keys

    void updateGeometries() override {
        if (_header && !_recursing) {
            _recursing = true;
            int height = _header->isHidden() ? 0 : _header->sizeHint().height();
            setViewportMargins(0, height, 0, 0);
            QRect area = viewport()->geometry();
            _header->setGeometry(area.left(), area.top() - height, area.width(), height);
            _recursing = false;
        }
        QListView::updateGeometries();
    }

private:
    QHeaderView* _header { nullptr };
    QPoint _pressPos;
    bool _pressedOnItem { false };
    bool _dragDone { false };
    bool _recursing { false };
};

// The folder tree: a QTreeView whose letters are not a type-ahead search, so they can reach the editor's keys.
class AssetTreeView : public QTreeView {
public:
    explicit AssetTreeView(QWidget* parent = nullptr) : QTreeView(parent) {}
    void keyboardSearch(const QString&) override {}
};

// --- AssetItemModel -----------------------------------------------------------------------------

AssetItemModel::AssetItemModel(QObject* parent) : QStandardItemModel(parent) {
}

QStringList AssetItemModel::mimeTypes() const {
    return { QString::fromLatin1(ASSET_MIME_TYPE), QStringLiteral("text/uri-list") };
}

QMimeData* AssetItemModel::mimeData(const QModelIndexList& indexes) const {
    auto data = new QMimeData();
    QList<QUrl> urls;
    QJsonArray assets;
    for (const auto& index : indexes) {
        if (index.column() != 0) {
            continue;
        }
        QJsonObject asset;
        asset["id"] = index.data(IdRole).toString();
        asset["path"] = index.data(PathRole).toString();
        asset["name"] = index.data(NameRole).toString();
        asset["kind"] = kindText((UserFile::Kind)index.data(KindRole).toInt());
        asset["url"] = index.data(UrlRole).toString();
        assets << asset;
        QString url = index.data(UrlRole).toString();
        if (!url.isEmpty()) {
            urls << QUrl(url);
        }
    }
    data->setData(QString::fromLatin1(ASSET_MIME_TYPE), QJsonDocument(assets).toJson(QJsonDocument::Compact));
    if (!urls.isEmpty()) {
        data->setUrls(urls);   // text/uri-list: what URL fields, picture fields and the viewport take
        QStringList texts;
        for (const auto& url : urls) {
            texts << url.toString();
        }
        data->setText(texts.join('\n'));
    }
    return data;
}

bool AssetItemModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const {
    if (!parent.isValid() && _dropFolder.isEmpty()) {
        return false;
    }
    if (parent.isValid() && !parent.data(FolderRole).toBool()) {
        return false;
    }
    if (data->hasFormat(QString::fromLatin1(ASSET_MIME_TYPE))) {
        return true;
    }
    for (const auto& url : data->urls()) {
        if (url.isLocalFile()) {
            return true;
        }
    }
    return false;
}

bool AssetItemModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    QString folder = parent.isValid() ? parent.data(PathRole).toString() : _dropFolder;
    emit dropped(data, folder);
    return true;
}

// --- AssetsPanel --------------------------------------------------------------------------------

AssetsPanel::AssetsPanel(QWidget* parent) : QDockWidget("My Files", parent) {
    setObjectName("EditorAssets");

    setFeatures(DockWidgetClosable | DockWidgetMovable | DockWidgetFloatable);   // anywhere, see HierarchyPanel
    DockTitleBar::attach(this);
    setMinimumHeight(150);

    _files = new UserFiles(this);
    _thumbnails = new AssetThumbnails(this);

    auto body = new QWidget(this);
    auto layout = new QVBoxLayout(body);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    _banner = new QLabel(body);
    _banner->setObjectName("EditorReadOnlyBanner");
    _banner->setWordWrap(true);
    _banner->hide();
    layout->addWidget(_banner);

    // the row above the views: where you are, then the search, the kind, and the buttons
    auto top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(4);
    _crumbs = new QWidget(body);
    _crumbLayout = new QHBoxLayout(_crumbs);
    _crumbLayout->setContentsMargins(0, 0, 0, 0);
    _crumbLayout->setSpacing(0);
    top->addWidget(_crumbs, 1);
    _search = new QLineEdit(body);
    _search->setPlaceholderText("Search");
    _search->setClearButtonEnabled(true);
    _search->setFixedWidth(180);
    top->addWidget(_search);
    _kind = new QComboBox(body);
    _kind->addItem("All", -1);
    _kind->addItem("Pictures", (int)UserFile::Kind::Picture);
    _kind->addItem("Models", (int)UserFile::Kind::Model);
    _kind->addItem("Avatars", (int)UserFile::Kind::Avatar);
    _kind->addItem("Sounds", (int)UserFile::Kind::Sound);
    _kind->addItem("Scripts", (int)UserFile::Kind::Script);
    _kind->addItem("Other", (int)UserFile::Kind::Other);
    top->addWidget(_kind);
    const char* viewTips[2] = { "Icons", "Details: the name in full, with the date, the size and the id" };
    for (int i = 0; i < 2; ++i) {
        auto button = new QToolButton(body);
        button->setObjectName("EditorAssetsView");
        button->setIcon(viewIcon(i == (int)View::Details));
        button->setIconSize(QSize(16, 16));
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setToolTip(viewTips[i]);
        connect(button, &QToolButton::clicked, this, [this, i] { setView((View)i); });
        top->addWidget(button);
        _viewButtons[i] = button;
    }
    auto upload = new QToolButton(body);
    upload->setText("Upload…");
    upload->setToolTip("Files from this computer, into this folder");
    connect(upload, &QToolButton::clicked, this, &AssetsPanel::chooseUploads);
    top->addWidget(upload);
    auto newFolderButton = new QToolButton(body);
    newFolderButton->setText("New Folder");
    connect(newFolderButton, &QToolButton::clicked, this, [this] { newFolder(_folder); });
    top->addWidget(newFolderButton);
    auto refresh = new QToolButton(body);
    refresh->setText("Refresh");
    connect(refresh, &QToolButton::clicked, _files, &UserFiles::refresh);
    top->addWidget(refresh);
    _quota = new QLabel(body);
    _quota->setObjectName("EditorAssetsQuota");
    top->addWidget(_quota);
    layout->addLayout(top);

    // the folder tree beside the folder's contents
    auto splitter = new QSplitter(Qt::Horizontal, body);
    splitter->setChildrenCollapsible(false);
    _treeModel = new AssetItemModel(this);
    _tree = new AssetTreeView(splitter);
    _tree->setObjectName("EditorAssetsTree");
    _tree->setModel(_treeModel);
    _tree->setHeaderHidden(true);
    _tree->setUniformRowHeights(true);
    _tree->setIndentation(14);
    _tree->setFrameShape(QFrame::NoFrame);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    _tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    _tree->setDragEnabled(true);
    _tree->setAcceptDrops(true);
    _tree->setDropIndicatorShown(true);
    _tree->setDragDropMode(QAbstractItemView::DragDrop);
    _tree->setDragDropOverwriteMode(true);   // drops land on a folder, never between rows
    _tree->setDefaultDropAction(Qt::CopyAction);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    _tree->setMinimumWidth(140);
    splitter->addWidget(_tree);

    _gridModel = new AssetItemModel(this);
    _grid = new AssetGridView(splitter);
    _grid->setObjectName("EditorAssetsGrid");
    _grid->setModel(_gridModel);
    _grid->setViewMode(QListView::IconMode);
    _grid->setFlow(QListView::LeftToRight);
    _grid->setWrapping(true);
    _grid->setResizeMode(QListView::Adjust);
    _grid->setMovement(QListView::Static);
    _grid->setIconSize(QSize(ICON_PIXELS, ICON_PIXELS));
    _grid->setGridSize(QSize(ICON_PIXELS + 36, ICON_PIXELS + 40));
    _grid->setSpacing(2);
    _grid->setUniformItemSizes(true);
    _grid->setWordWrap(true);
    _grid->setTextElideMode(Qt::ElideMiddle);
    _grid->setFrameShape(QFrame::NoFrame);
    _grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _grid->setSelectionRectVisible(true);
    _grid->setEditTriggers(QAbstractItemView::EditKeyPressed);
    _grid->setDragEnabled(true);
    _grid->setAcceptDrops(true);
    _grid->setDropIndicatorShown(true);
    _grid->setDragDropMode(QAbstractItemView::DragDrop);
    _grid->setDragDropOverwriteMode(true);
    _grid->setDefaultDropAction(Qt::CopyAction);
    _grid->setContextMenuPolicy(Qt::CustomContextMenu);
    _grid->installEventFilter(this);
    _grid->viewport()->installEventFilter(this);
    _tree->installEventFilter(this);   // only now: the filter asks the grid for its viewport
    _iconDelegate = _grid->itemDelegate();
    _detailsDelegate = new AssetDetailsDelegate(this);
    // the Details view's header: the columns, each clicked to sort by it
    _headerModel = new QStandardItemModel(0, ColumnCount, this);
    _headerModel->setHorizontalHeaderLabels({ "Name", "Kind", "Date", "Size", "Id" });
    _headerModel->setHeaderData(DateColumn, Qt::Horizontal, Qt::DescendingOrder, Qt::InitialSortOrderRole);   // newest first
    _headerModel->setHeaderData(SizeColumn, Qt::Horizontal, Qt::DescendingOrder, Qt::InitialSortOrderRole);   // biggest first
    _header = new QHeaderView(Qt::Horizontal, _grid);
    _header->setObjectName("EditorAssetsHeader");
    _header->setModel(_headerModel);
    _header->setSectionsMovable(false);
    _header->setSectionsClickable(true);
    _header->setHighlightSections(false);
    _header->setSortIndicatorShown(true);
    _header->setStretchLastSection(false);
    _header->setMinimumSectionSize(40);
    _header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    _header->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    _header->resizeSection(KindColumn, 76);
    _header->resizeSection(DateColumn, 140);
    _header->resizeSection(SizeColumn, 80);
    _header->resizeSection(IdColumn, 150);
    _header->hide();
    _detailsDelegate->setHeader(_header);
    _grid->setHeader(_header);
    connect(_header, &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) {
        setSort((Column)column, order == Qt::DescendingOrder);
    });
    splitter->addWidget(_grid);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 200, 800 });
    layout->addWidget(splitter, 1);

    // under the files, where they're dropped: the rights line, while signed in
    _note = new QLabel(body);
    _note->setObjectName("EditorAssetsNote");
    _note->setTextFormat(Qt::PlainText);
    _note->setVisible(false);
    layout->addWidget(_note);
    _rightsLine = rights::label("EditorRightsLine", QColor("#6fa8dc"), body);
    layout->addWidget(_rightsLine);

    _empty = new QLabel(_grid->viewport());
    _empty->setObjectName("EditorAssetsEmpty");
    _empty->setAlignment(Qt::AlignCenter);
    _empty->setWordWrap(true);
    _empty->setAttribute(Qt::WA_TransparentForMouseEvents);
    _empty->hide();

    setWidget(body);

    // the listing
    connect(_files, &UserFiles::listed, this, &AssetsPanel::onListed);
    connect(_files, &UserFiles::quotaChanged, this, &AssetsPanel::updateQuota);
    connect(_files, &UserFiles::failed, this, [this](const QString& what) { emit status(what, STATUS_MS * 2); });
    auto accountManager = DependencyManager::get<AccountManager>();
    connect(accountManager.data(), &AccountManager::loginComplete, this, [this] {
        updateBanner();
        if (_active) {
            _files->refresh();
        }
    });
    connect(accountManager.data(), &AccountManager::logoutComplete, this, &AssetsPanel::updateBanner);
    _refreshTimer.setSingleShot(true);
    _refreshTimer.setInterval(300);
    connect(&_refreshTimer, &QTimer::timeout, _files, &UserFiles::refresh);

    // the tree: the chosen folder is shown; folders renamed in place; drops move or upload into a folder
    connect(_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
        if (_updating || !current.isValid()) {
            return;
        }
        _search->clear();
        setFolder(current.data(AssetItemModel::PathRole).toString());
    });
    connect(_tree, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        if (!_updating) {
            _expanded.insert(index.data(AssetItemModel::PathRole).toString());
        }
    });
    connect(_tree, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        if (!_updating) {
            _expanded.remove(index.data(AssetItemModel::PathRole).toString());
        }
    });
    connect(_treeModel, &QStandardItemModel::itemChanged, this, [this](QStandardItem* item) {
        if (!_updating) {
            renameFromItem(item);
        }
    });
    connect(_treeModel, &AssetItemModel::dropped, this, &AssetsPanel::onDropped);
    connect(_tree, &QTreeView::customContextMenuRequested, this, [this](const QPoint& pos) {
        QModelIndex index = _tree->indexAt(pos);
        if (index.isValid()) {
            _tree->setCurrentIndex(index);
        }
        showTreeMenu(_tree->viewport()->mapToGlobal(pos));
    });

    // the grid
    connect(_gridModel, &QStandardItemModel::itemChanged, this, [this](QStandardItem* item) {
        if (!_updating) {
            renameFromItem(item);
        }
    });
    connect(_gridModel, &AssetItemModel::dropped, this, &AssetsPanel::onDropped);
    connect(_grid, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        const UserFile* file = _files->find(index.data(AssetItemModel::PathRole).toString());
        if (!file) {
            return;
        }
        if (file->folder) {
            _search->clear();
            setFolder(file->path);
        } else {
            openFile(*file);
        }
    });
    connect(_grid, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        // a right click on an item outside the selection selects that item first, as Unity does
        QModelIndex index = _grid->indexAt(pos);
        if (index.isValid() && !_grid->selectionModel()->isSelected(index)) {
            _grid->setCurrentIndex(index);
        }
        showGridMenu(_grid->viewport()->mapToGlobal(pos));
    });
    connect(_grid->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { _thumbnailTimer.start(); });
    _thumbnailTimer.setSingleShot(true);
    _thumbnailTimer.setInterval(60);
    connect(&_thumbnailTimer, &QTimer::timeout, this, &AssetsPanel::requestThumbnails);
    connect(_thumbnails, &AssetThumbnails::ready, this, [this](const QString& id) {
        QStandardItem* item = _gridById.value(id);
        const UserFile* file = item ? _files->find(item->data(AssetItemModel::PathRole).toString()) : nullptr;
        if (!file) {
            return;
        }
        QPixmap picture = _thumbnails->get(*file);
        if (!picture.isNull()) {
            _updating = true;
            item->setIcon(pictureIcon(picture, file->isPrivate));
            item->setData(true, AssetItemModel::PictureRole);
            _updating = false;
        }
    });

    // the search: a pause after typing, then every matching file in the whole tree
    _searchTimer.setSingleShot(true);
    _searchTimer.setInterval(150);
    connect(&_searchTimer, &QTimer::timeout, this, &AssetsPanel::rebuildGrid);
    connect(_search, &QLineEdit::textChanged, this, [this] { _searchTimer.start(); });
    connect(_kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { rebuildGrid(); });

    rebuildCrumbs();
    updateBanner();
    // as last chosen
    setSort((Column)qBound(0, _sortSetting.get(), (int)ColumnCount - 1), _sortDescendingSetting.get());
    setView((View)qBound(0, _viewSetting.get(), 1));
}

void AssetsPanel::setSort(Column column, bool descending) {
    _sortColumn = column;
    _sortDescending = descending;
    _sortSetting.set((int)column);
    _sortDescendingSetting.set(descending);
    Qt::SortOrder order = descending ? Qt::DescendingOrder : Qt::AscendingOrder;
    if (_header->sortIndicatorSection() != (int)column || _header->sortIndicatorOrder() != order) {
        _header->setSortIndicator((int)column, order);   // tells us again, with nothing left to change
    }
    rebuildGrid();
}

void AssetsPanel::setView(View view) {
    _view = view;
    _viewSetting.set((int)view);
    if (!_viewButtons[(int)view]->isChecked()) {
        _viewButtons[(int)view]->setChecked(true);
    }
    if (view == View::Icons) {
        if (_grid->itemDelegate() != _iconDelegate) {
            _grid->setItemDelegate(_iconDelegate);
        }
        _grid->setViewMode(QListView::IconMode);
        _grid->setFlow(QListView::LeftToRight);
        _grid->setWrapping(true);
        _grid->setSpacing(2);
        _grid->setIconSize(QSize(ICON_PIXELS, ICON_PIXELS));
        _grid->setGridSize(QSize(ICON_PIXELS + 36, ICON_PIXELS + 40));
        _grid->setWordWrap(true);
        _grid->setHeaderShown(false);
    } else {
        if (_grid->itemDelegate() != _detailsDelegate) {
            _grid->setItemDelegate(_detailsDelegate);
        }
        _grid->setViewMode(QListView::ListMode);
        _grid->setFlow(QListView::TopToBottom);
        _grid->setWrapping(false);
        _grid->setSpacing(0);
        _grid->setGridSize(QSize());   // rows are as the delegate sizes them
        _grid->setIconSize(QSize(AssetDetailsDelegate::ICON_PIXELS, AssetDetailsDelegate::ICON_PIXELS));
        _grid->setWordWrap(false);
        _grid->setHeaderShown(true);
    }
    // setViewMode resets these to its own defaults where they were never set
    _grid->setMovement(QListView::Static);
    _grid->setResizeMode(QListView::Adjust);
    _grid->doItemsLayout();
    placeEmptyNote();
    _thumbnailTimer.start();
}

void AssetsPanel::setActive(bool active) {
    _active = active;
    if (active) {
        updateBanner();
        // a listing of a minute or less is still good: a large My Files took the main thread for a while at each one,
        // and build mode asked for one every time it came on (the panel's Refresh, and its own changes, list at once)
        _files->refreshIfStale(60 * 1000);
    }
}

void AssetsPanel::setNote(const QString& text) {
    _note->setText(text);
    _note->setVisible(!text.isEmpty());
}

bool AssetsPanel::searching() const {
    return !_search->text().trimmed().isEmpty();
}

void AssetsPanel::updateBanner() {
    bool loggedIn = UserFiles::isLoggedIn();
    _banner->setText("Log in to see your files");
    _banner->setVisible(!loggedIn);
    _rightsLine->setVisible(loggedIn);
    _tree->setEnabled(loggedIn);
    _grid->setEnabled(loggedIn);
}

void AssetsPanel::updateQuota() {
    if (_files->maxSize() > 0) {
        _quota->setText(UserFile::sizeText(_files->usedSize()) + " of " + UserFile::sizeText(_files->maxSize()));
        _quota->setToolTip("Your files' space");
    } else {
        _quota->clear();
    }
}

// --- the listing into the views -----------------------------------------------------------------

void AssetsPanel::onListed() {
    // the folder shown may have gone (deleted, renamed): the nearest folder that exists is shown instead
    while (_folder != QStringLiteral("/") && !_files->find(_folder)) {
        _folder = UserFile::folderOf(_folder);
    }
    QElapsedTimer rebuildTimer;
    rebuildTimer.start();
    rebuildTree();
    rebuildCrumbs();
    rebuildGrid();
    updateQuota();
    qCDebug(interfaceapp) << "My Files shown in" << rebuildTimer.elapsed() << "ms";
    if (!_editAfterListing.isEmpty()) {
        QStandardItem* item = _gridItems.value(_editAfterListing);
        _editAfterListing.clear();
        if (item) {
            _grid->setCurrentIndex(item->index());
            _grid->edit(item->index());
        }
    }
}

QIcon AssetsPanel::typeIcon(const UserFile& file) const {
    QString suffix = file.folder ? QString() : QFileInfo(file.name).suffix().toLower();
    QString key = QString("%1|%2|%3").arg((int)file.kind).arg(suffix).arg(file.isPrivate ? 1 : 0);
    auto found = _typeIcons.find(key);
    if (found != _typeIcons.end()) {
        return *found;
    }
    QIcon icon;
    icon.addPixmap(paintTypeIcon(file.kind, suffix, file.isPrivate, ICON_PIXELS));
    icon.addPixmap(paintTypeIcon(file.kind, suffix, file.isPrivate, ICON_PIXELS * 2));   // for a Retina display
    _typeIcons.insert(key, icon);
    return icon;
}

QIcon AssetsPanel::pictureIcon(const QPixmap& picture, bool isPrivate) const {
    // the picture in the icon's square, on nothing, with a thin frame
    const int pixels = AssetThumbnails::PIXELS;
    QPixmap framed(pixels, pixels);
    framed.fill(Qt::transparent);
    QPainter painter(&framed);
    QPixmap scaled = picture.scaled(pixels - 4, pixels - 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QRect target((pixels - scaled.width()) / 2, (pixels - scaled.height()) / 2, scaled.width(), scaled.height());
    painter.drawPixmap(target, scaled);
    painter.setPen(QColor(0, 0, 0, 120));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
    if (isPrivate) {
        paintLock(painter, QRectF(pixels * 0.64, pixels * 0.64, pixels * 0.3, pixels * 0.3));
    }
    painter.end();
    QIcon icon;
    icon.addPixmap(framed);
    return icon;
}

void AssetsPanel::fillItem(QStandardItem* item, const UserFile& file) const {
    item->setText(file.name);
    item->setData(file.path, AssetItemModel::PathRole);
    item->setData(file.name, AssetItemModel::NameRole);
    item->setData(file.id, AssetItemModel::IdRole);
    item->setData(file.url, AssetItemModel::UrlRole);
    item->setData(file.pathUrl, AssetItemModel::PathUrlRole);
    item->setData(file.folder, AssetItemModel::FolderRole);
    item->setData((int)file.kind, AssetItemModel::KindRole);
    item->setData(file.isPrivate, AssetItemModel::PrivateRole);
    item->setData(false, AssetItemModel::PictureRole);
    item->setData(file.size, AssetItemModel::SizeRole);
    item->setData(file.updated, AssetItemModel::UpdatedRole);
    if (file.folder) {
        item->setData(_files->fileCountUnder(file.path), AssetItemModel::FileCountRole);
    }
    item->setEditable(true);
    item->setDragEnabled(true);
    item->setDropEnabled(file.folder);
    QStringList lines { file.name };
    QString about = kindText(file.kind);
    if (!file.folder) {
        about += " · " + UserFile::sizeText(file.size);
    }
    if (file.updated.isValid()) {
        about += " · " + file.updated.toString("d MMM yyyy HH:mm");
    }
    if (file.isPrivate) {
        about += " · private";
    }
    lines << about << file.path;
    item->setToolTip(lines.join('\n'));
    QPixmap picture = file.kind == UserFile::Kind::Picture ? _thumbnails->get(file) : QPixmap();
    if (!picture.isNull()) {
        item->setIcon(pictureIcon(picture, file.isPrivate));
        item->setData(true, AssetItemModel::PictureRole);
    } else {
        item->setIcon(typeIcon(file));
    }
}

void AssetsPanel::rebuildTree() {
    _updating = true;
    _treeModel->clear();
    _treeItems.clear();
    auto root = new QStandardItem(ROOT_LABEL);
    UserFile rootFile;
    rootFile.path = "/";
    rootFile.name = ROOT_LABEL;
    rootFile.folder = true;
    rootFile.kind = UserFile::Kind::Folder;
    fillItem(root, rootFile);
    root->setEditable(false);
    root->setDragEnabled(false);
    root->setToolTip("Your files on " + DependencyManager::get<AccountManager>()->getMetaverseServerURL().host());
    _treeModel->appendRow(root);
    _treeItems.insert("/", root);
    std::function<void(QStandardItem*, const QString&)> addFolders = [&](QStandardItem* parent, const QString& folderPath) {
        for (const UserFile* child : _files->childrenOf(folderPath)) {
            if (!child->folder) {
                continue;
            }
            auto item = new QStandardItem();
            fillItem(item, *child);
            parent->appendRow(item);
            _treeItems.insert(child->path, item);
            addFolders(item, child->path);
        }
    };
    addFolders(root, "/");
    for (const auto& path : _expanded) {
        if (QStandardItem* item = _treeItems.value(path)) {
            _tree->expand(item->index());
        }
    }
    if (QStandardItem* current = _treeItems.value(_folder)) {
        _tree->setCurrentIndex(current->index());
    }
    _updating = false;
}

void AssetsPanel::sortFiles(QVector<const UserFile*>& files) const {
    // folders first whatever the order; then by the column, and by name among equals
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    auto compare = [&](const UserFile* a, const UserFile* b) -> int {
        switch (_sortColumn) {
            case KindColumn: return a->kind == b->kind ? 0 : UserFile::kindName(a->kind) < UserFile::kindName(b->kind) ? -1 : 1;
            case DateColumn: return a->updated == b->updated ? 0 : a->updated < b->updated ? -1 : 1;
            case SizeColumn: return a->size == b->size ? 0 : a->size < b->size ? -1 : 1;
            case IdColumn: return a->id.compare(b->id);
            default: return 0;
        }
    };
    std::stable_sort(files.begin(), files.end(), [&](const UserFile* a, const UserFile* b) {
        if (a->folder != b->folder) {
            return a->folder;
        }
        int order = compare(a, b);
        if (order == 0) {
            order = collator.compare(a->name, b->name);
        }
        return _sortDescending ? order > 0 : order < 0;
    });
}

void AssetsPanel::rebuildGrid() {
    // what was selected stays selected, by path
    QSet<QString> selected;
    for (const auto& index : _grid->selectionModel()->selectedIndexes()) {
        selected.insert(index.data(AssetItemModel::PathRole).toString());
    }
    _updating = true;
    _gridModel->clear();
    _gridItems.clear();
    _gridById.clear();
    int kindWanted = _kind->currentData().toInt();
    auto kindMatches = [kindWanted](const UserFile& file) {
        return kindWanted < 0 || file.folder || (int)file.kind == kindWanted;
    };
    QVector<const UserFile*> shown;
    bool capped = false;
    if (searching()) {
        QString text = _search->text().trimmed();
        // a pasted stable link names its id; a pasted path link names its path
        QString wantedId;
        QString wantedPath;
        QRegularExpression idLink("/f/([A-Za-z0-9]{16})(?:/|$)");
        auto idMatch = idLink.match(text);
        if (idMatch.hasMatch()) {
            wantedId = idMatch.captured(1);
        } else if (text.startsWith("http://") || text.startsWith("https://")) {
            QUrl link(text);
            wantedPath = QUrl::fromPercentEncoding(link.path().section('/', 2).toUtf8());   // after "/<username>"
            if (!wantedPath.startsWith('/')) {
                wantedPath.prepend('/');
            }
        }
        auto matches = [&](const UserFile& file) {
            if (!wantedId.isEmpty()) {
                return file.id == wantedId;
            }
            if (!wantedPath.isEmpty()) {
                return file.path.compare(wantedPath, Qt::CaseInsensitive) == 0;
            }
            return file.name.contains(text, Qt::CaseInsensitive) || (!file.id.isEmpty() && file.id.contains(text, Qt::CaseInsensitive));
        };
        for (const auto& file : _files->files()) {
            if (matches(file) && kindMatches(file)) {
                if (shown.size() >= MAX_SEARCH_RESULTS) {
                    capped = true;
                    break;
                }
                shown << &file;
            }
        }
        _gridModel->setDropFolder(QString());   // a search result's folder is not the folder shown
        _detailsDelegate->setShowsFolder(true);
    } else {
        for (const UserFile* file : _files->childrenOf(_folder)) {
            if (kindMatches(*file)) {
                shown << file;
            }
        }
        _gridModel->setDropFolder(_folder);
        _detailsDelegate->setShowsFolder(false);
    }
    sortFiles(shown);
    QItemSelection reselect;
    for (const UserFile* file : shown) {
        auto item = new QStandardItem();
        fillItem(item, *file);
        _gridModel->appendRow(item);
        _gridItems.insert(file->path, item);
        if (!file->id.isEmpty()) {
            _gridById.insert(file->id, item);
        }
        if (selected.contains(file->path)) {
            reselect.select(item->index(), item->index());
        }
    }
    if (!reselect.isEmpty()) {
        _grid->selectionModel()->select(reselect, QItemSelectionModel::ClearAndSelect);
    }
    _updating = false;

    if (!_files->hasListing()) {
        _empty->setText(UserFiles::isLoggedIn() ? (_files->isLoading() ? "Loading…" : QString()) : QString());
    } else if (shown.isEmpty()) {
        _empty->setText(searching() ? "Nothing matches" : "Empty folder\nDrop files here to upload them");
    } else {
        _empty->setText(capped ? QString("The first %1 matches").arg(MAX_SEARCH_RESULTS) : QString());
    }
    _empty->setVisible(!_empty->text().isEmpty());
    placeEmptyNote();
    _thumbnailTimer.start();
}

void AssetsPanel::placeEmptyNote() {
    QRect area = _grid->viewport()->rect();
    if (_gridModel->rowCount() == 0) {
        _empty->setGeometry(area.adjusted(12, 12, -12, -12));
    } else {
        _empty->setGeometry(area.left() + 8, area.bottom() - 24, area.width() - 16, 20);   // a note along the bottom
    }
}

void AssetsPanel::rebuildCrumbs() {
    while (QLayoutItem* item = _crumbLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QStringList segments = _folder.split('/', Qt::SkipEmptyParts);
    QString path = "/";
    auto addCrumb = [this](const QString& label, const QString& target, bool last) {
        auto button = new QToolButton(_crumbs);
        button->setObjectName("EditorAssetsCrumb");
        button->setText(label);
        button->setAutoRaise(true);
        button->setProperty("last", last);
        button->setToolTip(target);
        connect(button, &QToolButton::clicked, this, [this, target] {
            _search->clear();
            setFolder(target);
        });
        _crumbLayout->addWidget(button);
    };
    addCrumb(ROOT_LABEL, "/", segments.isEmpty());
    for (int i = 0; i < segments.size(); ++i) {
        path += segments[i] + "/";
        auto separator = new QLabel(QStringLiteral("›"), _crumbs);
        separator->setObjectName("EditorAssetsCrumbSeparator");
        _crumbLayout->addWidget(separator);
        addCrumb(segments[i], path, i == segments.size() - 1);
    }
    _crumbLayout->addStretch(1);
}

void AssetsPanel::setFolder(const QString& folderPath) {
    QString wanted = folderPath.isEmpty() ? QStringLiteral("/") : folderPath;
    if (wanted != QStringLiteral("/") && !_files->find(wanted)) {
        return;
    }
    _folder = wanted;
    // the tree follows, its rows above opened
    for (QString above = UserFile::folderOf(_folder); !above.isEmpty(); above = UserFile::folderOf(above)) {
        _expanded.insert(above);
    }
    _updating = true;
    if (QStandardItem* item = _treeItems.value(_folder)) {
        for (QModelIndex parent = item->index().parent(); parent.isValid(); parent = parent.parent()) {
            _tree->expand(parent);
        }
        _tree->setCurrentIndex(item->index());
        _tree->scrollTo(item->index());
    }
    _updating = false;
    rebuildCrumbs();
    rebuildGrid();
}

// --- thumbnails for what is in view -------------------------------------------------------------

void AssetsPanel::requestThumbnails() {
    if (!_grid->isVisible()) {
        return;
    }
    int rowHeight = _view == View::Icons ? _grid->gridSize().height() : AssetDetailsDelegate::ROW_HEIGHT;
    QRect visible = _grid->viewport()->rect().adjusted(0, -rowHeight, 0, rowHeight);
    QVector<UserFile> wanted;
    for (int row = 0; row < _gridModel->rowCount(); ++row) {
        QStandardItem* item = _gridModel->item(row);
        if ((UserFile::Kind)item->data(AssetItemModel::KindRole).toInt() != UserFile::Kind::Picture
                || item->data(AssetItemModel::PictureRole).toBool()) {
            continue;
        }
        if (!_grid->visualRect(item->index()).intersects(visible)) {
            continue;
        }
        if (const UserFile* file = _files->find(item->data(AssetItemModel::PathRole).toString())) {
            if (!_thumbnails->isMissing(*file)) {
                wanted << *file;
            }
        }
    }
    _thumbnails->want(wanted);
}

bool AssetsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (!_grid || !_tree) {
        return QDockWidget::eventFilter(watched, event);
    }
    if (watched == _grid->viewport() && event->type() == QEvent::Resize) {
        placeEmptyNote();
        _thumbnailTimer.start();
    }
    bool aView = watched == _grid || watched == _tree;
    if (aView && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
        // a name being typed in place has its own editor, which keeps its keys
        QWidget* focus = QApplication::focusWidget();
        auto view = static_cast<QWidget*>(watched);
        if (focus && focus != view && view->isAncestorOf(focus)) {
            return QDockWidget::eventFilter(watched, event);
        }
        auto keyEvent = static_cast<QKeyEvent*>(event);
        bool command = keyEvent->modifiers().testFlag(Qt::ControlModifier) || keyEvent->modifiers().testFlag(Qt::MetaModifier);
        // the editor's own keys (the tools, the handle space, Frame, vertex snapping while V is held) work from
        // here too, as the panel has no use for them
        static const QSet<int> EDITOR_KEYS { Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R, Qt::Key_T, Qt::Key_X, Qt::Key_Z,
                                             Qt::Key_F, Qt::Key_V, Qt::Key_1, Qt::Key_2, Qt::Key_3,
                                             Qt::Key_BracketLeft, Qt::Key_BracketRight };
        if (!command && EDITOR_KEYS.contains(keyEvent->key()) && _keyFallback && _keyFallback(keyEvent)) {
            return true;
        }
        if (watched != _grid || event->type() != QEvent::KeyPress) {
            return QDockWidget::eventFilter(watched, event);
        }
        switch (keyEvent->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                // Enter renames, as in the Finder; a double-click opens
                if (_grid->currentIndex().isValid() && !command) {
                    _grid->edit(_grid->currentIndex());
                    return true;
                }
                break;
            case Qt::Key_Delete:
            case Qt::Key_Backspace:
                deleteFiles(selectedFiles());
                return true;
            case Qt::Key_C:
                if (command) {
                    copyLinks(selectedFiles(), keyEvent->modifiers().testFlag(Qt::ShiftModifier));
                    return true;
                }
                break;
            case Qt::Key_D:
                if (command) {
                    duplicateFiles(selectedFiles());
                    return true;
                }
                break;
            case Qt::Key_Up:
                // Command+Up: the folder above, as in the Finder
                if (command && _folder != QStringLiteral("/") && !searching()) {
                    setFolder(UserFile::folderOf(_folder));
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

// --- operations ---------------------------------------------------------------------------------

QList<const UserFile*> AssetsPanel::selectedFiles() const {
    QList<const UserFile*> files;
    for (const auto& index : _grid->selectionModel()->selectedIndexes()) {
        if (const UserFile* file = _files->find(index.data(AssetItemModel::PathRole).toString())) {
            files << file;
        }
    }
    return files;
}

const UserFile* AssetsPanel::treeFolderAt(const QModelIndex& index) const {
    return index.isValid() ? _files->find(index.data(AssetItemModel::PathRole).toString()) : nullptr;
}

void AssetsPanel::newFolder(const QString& inFolder) {
    if (!UserFiles::isLoggedIn()) {
        emit status("Log in to make folders", STATUS_MS);
        return;
    }
    QString path = inFolder + _files->freeName(inFolder, "New Folder") + "/";
    _editAfterListing = path;   // named in place once it is listed, as Unity does
    if (!searching() && inFolder == _folder) {
        // nothing: the grid shows it
    } else {
        _search->clear();
        _folder = inFolder;
    }
    _files->createFolder(path);
}

void AssetsPanel::renameFromItem(QStandardItem* item) {
    QString path = item->data(AssetItemModel::PathRole).toString();
    QString was = item->data(AssetItemModel::NameRole).toString();
    QString now = item->text().trimmed();
    auto revert = [&] {
        _updating = true;
        item->setText(was);
        _updating = false;
    };
    if (now == was || path.isEmpty() || path == QStringLiteral("/")) {
        revert();
        return;
    }
    if (now.isEmpty() || now.contains('/') || now == QStringLiteral(".") || now == QStringLiteral("..")) {
        revert();
        emit status("A name can't be empty or contain a slash", STATUS_MS);
        return;
    }
    bool folder = path.endsWith('/');
    QString newPath = UserFile::folderOf(path) + now + (folder ? "/" : "");
    if (_files->find(newPath)) {
        revert();
        emit status("There is already a " + QString(folder ? "folder" : "file") + " called " + now + " here", STATUS_MS);
        return;
    }
    if (folder && _folder.startsWith(path)) {
        _folder = newPath + _folder.mid(path.length());   // the folder shown is being renamed: follow it
    }
    _files->move(path, newPath);
}

void AssetsPanel::deleteFiles(const QList<const UserFile*>& files) {
    if (files.isEmpty()) {
        return;
    }
    QString question;
    int folders = 0;
    int inside = 0;
    for (const UserFile* file : files) {
        if (file->folder) {
            ++folders;
            inside += _files->fileCountUnder(file->path);
        }
    }
    if (files.size() == 1) {
        const UserFile* file = files.first();
        question = file->folder
            ? QString("Delete the folder \"%1\" and the %2 in it?").arg(file->name).arg(inside == 1 ? "file" : QString("%1 files").arg(inside))
            : QString("Delete \"%1\"?").arg(file->name);
    } else {
        question = QString("Delete these %1 items?").arg(files.size());
        if (folders > 0) {
            question += QString(" The %1 hold %2 files.").arg(folders == 1 ? "folder" : "folders").arg(inside);
        }
    }
    // Darkride's alert over the window (the dock's style sheet hid a box's highlight), with the warning and a Cancel
    // button: a box with Delete alone (1ba7996d09 lost these) made Delete its escape button, so Escape and closing
    // the box deleted the files. Delete is red; Return and Escape are Cancel.
    const bool one = files.size() == 1 && !files.first()->folder;
    popup::Alert alert(popup::appIcon(), question,
                       QString("Every world that uses %1 will lose %1. This can't be undone.").arg(one ? "it" : "them"),
                       QString(), { { "Cancel", popup::Role::Cancel }, { "Delete", popup::Role::Destructive } }, window());
    alert.exec();
    if (alert.answer() != 1) {
        return;
    }
    QStringList paths;
    for (const UserFile* file : files) {
        paths << file->path;   // the pointers die with the listing that a success brings back
    }
    for (const auto& path : paths) {
        _files->remove(path);
    }
}

void AssetsPanel::copyLinks(const QList<const UserFile*>& files, bool pathLinks) {
    QStringList links;
    for (const UserFile* file : files) {
        if (file->folder) {
            continue;
        }
        links << (pathLinks || file->url.isEmpty() ? file->pathUrl : file->url);
    }
    if (links.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(links.join('\n'));
    emit status(links.size() == 1 ? "Link copied" : QString("%1 links copied").arg(links.size()), STATUS_MS);
}

void AssetsPanel::copyIds(const QList<const UserFile*>& files) {
    QStringList ids;
    for (const UserFile* file : files) {
        if (!file->id.isEmpty()) {
            ids << file->id;
        }
    }
    if (ids.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(ids.join('\n'));
    emit status(ids.size() == 1 ? "Id copied" : QString("%1 ids copied").arg(ids.size()), STATUS_MS);
}

bool AssetsPanel::opensInCode(const UserFile& file) const {
    return !file.folder && !file.url.isEmpty() && CodeEditor::isAvailable() && CodeEditor::opensInCode(file.name) &&
           DependencyManager::isSet<CodeEditor>();
}

void AssetsPanel::openFile(const UserFile& file) {
    // a script opens in the Code panel (Caity 11-08, 1.1.4), a private one too; everything else in the browser for now
    // (the files viewer is 1.1.5's)
    if (opensInCode(file) && DependencyManager::get<CodeEditor>()->openMyFile(file.path, QUrl(file.url), file.name, file.size)) {
        return;
    }
    openInBrowser(file);
}

void AssetsPanel::openInBrowser(const UserFile& file) {
    QString url = file.url.isEmpty() ? file.pathUrl : file.url;
    if (!url.isEmpty()) {
        QDesktopServices::openUrl(QUrl(url));
    }
}

void AssetsPanel::chooseUploads() {
    if (!UserFiles::isLoggedIn()) {
        emit status("Log in to upload", STATUS_MS);
        return;
    }
    QStringList paths = QFileDialog::getOpenFileNames(this, "Upload to " + (_folder == "/" ? ROOT_LABEL : _folder), QDir::homePath());
    if (!paths.isEmpty()) {
        uploadFiles(paths, _folder);
    }
}

void AssetsPanel::uploadFiles(const QStringList& localPaths, const QString& folderPath) {
    QString remoteFolder = folderPath.endsWith('/') ? folderPath.chopped(1) : folderPath;   // the uploader adds "/name"
    int folders = 0;
    for (const auto& localPath : localPaths) {
        if (QFileInfo(localPath).isDir()) {
            ++folders;   // a dropped folder is not walked (yet): its files one by one
            continue;
        }
        auto upload = new EditorUpload(this);
        QString error;
        if (!upload->start(localPath, remoteFolder, error)) {
            emit status("Upload failed: " + error, STATUS_MS * 2);
            qCWarning(interfaceapp) << "assets upload could not start" << localPath << error;
            upload->deleteLater();
            continue;
        }
        trackUpload(upload, true);
    }
    if (folders > 0) {
        emit status(QString("%1 can't be uploaded whole yet: drop its files instead").arg(folders == 1 ? "A folder" : "Folders"), STATUS_MS * 2);
    }
}

void AssetsPanel::trackUpload(EditorUpload* upload, bool fromComputer) {
    ++_uploadsActive;
    emit status(EditorUpload::uploadingStatus(upload->fileName(), 0, QString(), fromComputer), 0);
    connect(upload, &EditorUpload::progress, this, [this, upload, fromComputer](qint64 sent, qint64 total) {
        if (total <= 0) {
            return;
        }
        QString more = _uploadsActive > 1 ? QString(" (+%1 more)").arg(_uploadsActive - 1) : QString();
        if (sent >= total) {
            emit status(EditorUpload::uploadingStatus(upload->fileName(), 100, more, fromComputer), 0);
        } else {
            emit status(EditorUpload::uploadingStatus(upload->fileName(), (int)(sent * 100 / total), more, fromComputer), 0);
        }
    });
    connect(upload, &EditorUpload::failed, this, [this, upload](const QString& error) {
        --_uploadsActive;
        emit status("Upload of " + upload->fileName() + " failed: " + error, STATUS_MS * 2);
        upload->deleteLater();
        _refreshTimer.start();
    });
    connect(upload, &EditorUpload::finished, this, [this, upload] {
        --_uploadsActive;
        emit status(_uploadsActive > 0 ? QString("Uploading…") : "Uploaded " + upload->fileName(), STATUS_MS);
        upload->deleteLater();
        _refreshTimer.start();   // the listing, once the batch has settled
    });
}

void AssetsPanel::duplicateFiles(const QList<const UserFile*>& files) {
    // the metaverse has no copy call: the bytes come down by the stable link and go up again under the new name
    int folders = 0;
    for (const UserFile* file : files) {
        if (file->folder) {
            ++folders;
            continue;
        }
        if (file->url.isEmpty()) {
            continue;
        }
        QString folder = UserFile::folderOf(file->path);
        QString remoteFolder = folder.endsWith('/') ? folder.chopped(1) : folder;   // the uploader adds "/name"
        // "a-copy.png", then "a-copy-2.png", ...: a name free in the folder, as the upload will store it
        QString suffix = QFileInfo(file->name).suffix();
        QString base = EditorUpload::safeFileName(suffix.isEmpty() ? file->name : file->name.chopped(suffix.length() + 1));
        QString extension = suffix.isEmpty() ? QString() : "." + suffix;
        QString name = base + "-copy" + extension;
        for (int i = 2; _files->find(folder + name); ++i) {
            name = QString("%1-copy-%2%3").arg(base).arg(i).arg(extension);
        }
        QNetworkRequest request{ QUrl(file->url) };
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = NetworkAccessManager::getInstance().get(request);
        ++_uploadsActive;
        emit status("Duplicating " + file->name + "…", 0);
        QString original = file->name;
        bool isPrivate = file->isPrivate;
        connect(reply, &QNetworkReply::finished, this, [this, reply, name, remoteFolder, original, isPrivate] {
            reply->deleteLater();
            --_uploadsActive;
            if (reply->error() != QNetworkReply::NoError) {
                QString why = isPrivate ? QStringLiteral("it is private, and private files do not reach the editor yet")
                                        : reply->errorString();
                emit status("Could not duplicate " + original + ": " + why, STATUS_MS * 2);
                qCWarning(interfaceapp) << "assets duplicate could not fetch" << original << reply->errorString();
                return;
            }
            QByteArray bytes = reply->readAll();
            QString mimeType = reply->header(QNetworkRequest::ContentTypeHeader).toString().section(';', 0, 0).trimmed();
            if (mimeType.isEmpty()) {
                mimeType = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
            }
            auto upload = new EditorUpload(this);
            QString error;
            if (!upload->startData(bytes, name, remoteFolder, mimeType, error)) {
                emit status("Could not duplicate " + original + ": " + error, STATUS_MS * 2);
                upload->deleteLater();
                return;
            }
            trackUpload(upload, false);   // a copy of a file already up
        });
    }
    if (folders > 0) {
        emit status(QString("%1 can't be duplicated yet").arg(folders == 1 ? "A folder" : "Folders"), STATUS_MS * 2);
    }
}

void AssetsPanel::onDropped(const QMimeData* data, const QString& folderPath) {
    if (data->hasFormat(QString::fromLatin1(AssetItemModel::ASSET_MIME_TYPE))) {
        // the panel's own files: a move into the folder
        QJsonArray assets = QJsonDocument::fromJson(data->data(QString::fromLatin1(AssetItemModel::ASSET_MIME_TYPE))).array();
        for (const auto& value : assets) {
            QJsonObject asset = value.toObject();
            QString path = asset.value("path").toString();
            QString name = asset.value("name").toString();
            if (path.isEmpty() || path == QStringLiteral("/") || UserFile::folderOf(path) == folderPath) {
                continue;
            }
            bool folder = path.endsWith('/');
            if (folder && folderPath.startsWith(path)) {
                emit status("A folder can't be moved into itself", STATUS_MS);
                continue;
            }
            QString newPath = folderPath + name + (folder ? "/" : "");
            if (_files->find(newPath)) {
                emit status("There is already something called " + name + " in " + (folderPath == "/" ? ROOT_LABEL : folderPath), STATUS_MS);
                continue;
            }
            if (folder && _folder.startsWith(path)) {
                _folder = newPath + _folder.mid(path.length());
            }
            _files->move(path, newPath);
        }
        return;
    }
    QStringList localPaths;
    for (const auto& url : data->urls()) {
        if (url.isLocalFile()) {
            localPaths << url.toLocalFile();
        }
    }
    if (!localPaths.isEmpty()) {
        uploadFiles(localPaths, folderPath);
    }
}

// --- menus --------------------------------------------------------------------------------------

void AssetsPanel::showGridMenu(const QPoint& globalPos) {
    QList<const UserFile*> files = selectedFiles();
    QMenu menu;   // not a child of the dock: its style sheet would paint over the menu's highlight
    if (!files.isEmpty()) {
        const UserFile* one = files.size() == 1 ? files.first() : nullptr;
        bool anyFile = false;
        for (const UserFile* file : files) {
            anyFile = anyFile || !file->folder;
        }
        if (one && one->folder) {
            menu.addAction("Open", this, [this, path = one->path] {
                _search->clear();
                setFolder(path);
            });
        }
        if (one && !one->folder && opensInCode(*one)) {
            menu.addAction("Open in Code", this, [this, file = *one] { openFile(file); });
        }
        if (one && !one->folder) {
            menu.addAction("Open in Browser", this, [this, file = *one] { openInBrowser(file); });
        }
        if (anyFile) {
            menu.addAction("Copy Link", this, [this, files] { copyLinks(files, false); })
                ->setToolTip("The stable link, which survives moves and renames");
            menu.addAction("Copy Path Link", this, [this, files] { copyLinks(files, true); })
                ->setToolTip("The link by folder and name");
            menu.addAction("Copy Id", this, [this, files] { copyIds(files); })
                ->setToolTip("The file's id alone: 16 letters and digits, fixed for its life");
        }
        if (anyFile) {
            menu.addAction("Duplicate", this, [this, files] { duplicateFiles(files); })
                ->setToolTip("A copy beside it, named -copy");
        }
        if (searching()) {
            menu.addAction("Show in Folder", this, [this, path = files.first()->path] {
                _search->clear();
                setFolder(UserFile::folderOf(path));
                if (QStandardItem* item = _gridItems.value(path)) {
                    _grid->setCurrentIndex(item->index());
                    _grid->scrollTo(item->index());
                }
            });
        }
        menu.addSeparator();
        if (one) {
            menu.addAction("Rename", this, [this, path = one->path] {
                if (QStandardItem* item = _gridItems.value(path)) {
                    _grid->edit(item->index());
                }
            });
            QAction* isPrivate = menu.addAction("Private", this, [this, path = one->path] { _files->togglePrivate(path); });
            isPrivate->setCheckable(true);
            isPrivate->setChecked(one->isPrivate);
            isPrivate->setToolTip("Only you can load a private file; a folder's files all at once");
        }
        menu.addAction("Delete…", this, [this, files] { deleteFiles(files); });
        menu.addSeparator();
    }
    menu.addAction("New Folder", this, [this] { newFolder(searching() ? QStringLiteral("/") : _folder); });
    menu.addAction("Upload…", this, &AssetsPanel::chooseUploads);
    menu.addAction("Refresh", _files, &UserFiles::refresh);
    menu.exec(globalPos);
}

void AssetsPanel::showTreeMenu(const QPoint& globalPos) {
    const UserFile* folder = treeFolderAt(_tree->currentIndex());
    QString path = folder ? folder->path : QStringLiteral("/");
    QMenu menu;   // not a child of the dock: its style sheet would paint over the menu's highlight
    menu.addAction("New Folder", this, [this, path] { newFolder(path); });
    if (folder && path != QStringLiteral("/")) {
        menu.addAction("Rename", this, [this, path] {
            if (QStandardItem* item = _treeItems.value(path)) {
                _tree->edit(item->index());
            }
        });
        QAction* isPrivate = menu.addAction("Private", this, [this, path] { _files->togglePrivate(path); });
        isPrivate->setCheckable(true);
        isPrivate->setChecked(folder->isPrivate);
        menu.addAction("Delete…", this, [this, folder] { deleteFiles({ folder }); });
    }
    menu.addSeparator();
    menu.addAction("Refresh", _files, &UserFiles::refresh);
    menu.exec(globalPos);
}
