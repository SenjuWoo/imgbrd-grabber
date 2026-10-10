#ifndef IMAGE_GRID_H
#define IMAGE_GRID_H

#include <QAbstractListModel>
#include <QCache>
#include <QHash>
#include <QListView>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QStyledItemDelegate>


class QTimer;

struct ImageGridItem
{
	QString key;
	QPixmap pixmap;
	QByteArray encoded; // Decoded on demand, so large Libraries stay light.
	QString title;
	QString tooltip;
	QString badge;
	bool liked = false;
	bool favorite = false;
	qint64 shownAt = 0;
};

class ImageGridModel : public QAbstractListModel
{
	Q_OBJECT

	public:
		enum Role { KeyRole = Qt::UserRole + 1, PixmapRole, LikedRole, FavoriteRole, BadgeRole, ShownAtRole };

		using QAbstractListModel::QAbstractListModel;
		int rowCount(const QModelIndex &parent = {}) const override;
		QVariant data(const QModelIndex &index, int role) const override;

		void setItems(const QList<ImageGridItem> &items);
		void append(const QList<ImageGridItem> &items);
		void clear();
		bool contains(const QString &key) const;
		int row(const QString &key) const;
		const ImageGridItem &item(int row) const;
		QStringList keys() const;
		void setPixmap(const QString &key, const QPixmap &pixmap);
		void setRating(const QString &key, bool liked, bool favorite);
		void remove(const QString &key);

	private:
		void reindex(int from);
		QList<ImageGridItem> m_items;
		QHash<QString, int> m_rows;
		mutable QCache<QString, QPixmap> m_decoded {96 * 1024};
};

class ImageGridView;

class ImageGridDelegate : public QStyledItemDelegate
{
	public:
		explicit ImageGridDelegate(ImageGridView *view);
		void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
		QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

	private:
		ImageGridView *m_view;
		mutable QCache<QString, QPixmap> m_scaled {64 * 1024};
};

/**
 * Responsive picture grid: tiles fill the width, previews are cropped to a 4:5 frame and fade in,
 * and Like / Favorite / Download / Hide appear only on selected pictures.
 */
class ImageGridView : public QListView
{
	Q_OBJECT

	public:
		enum Action { Like, Favorite, Download, Hide };
		Q_ENUM(Action)

		explicit ImageGridView(QWidget *parent = nullptr);
		ImageGridModel *gridModel() const;
		void setTargetWidth(int width);
		void setDensity(int density);
		QSize tileSize() const;
		void setActions(const QList<Action> &actions);
		const QList<Action> &actions() const;
		void setShowRatings(bool show);
		bool showRatings() const;
		QStringList selectedKeys() const;
		QString currentKey() const;
		QRect actionRect(const QRect &tile, int position) const;
		int actionAt(const QModelIndex &index, const QPoint &position) const;
		int hoveredAction(const QModelIndex &index) const;
		void startFade();
		void checkNearEnd();
		static int densityWidth(int density);

	signals:
		void actionTriggered(ImageGridView::Action action, const QStringList &keys);
		void openRequested(const QString &key);
		void nearEnd();
		void contextMenuRequested(const QStringList &keys, const QPoint &globalPosition);

	protected:
		void resizeEvent(QResizeEvent *event) override;
		void updateGeometries() override;
		void mousePressEvent(QMouseEvent *event) override;
		void mouseReleaseEvent(QMouseEvent *event) override;
		void mouseDoubleClickEvent(QMouseEvent *event) override;
		void mouseMoveEvent(QMouseEvent *event) override;
		void leaveEvent(QEvent *event) override;
		void keyPressEvent(QKeyEvent *event) override;
		void contextMenuEvent(QContextMenuEvent *event) override;

	private:
		void updateGrid();
		ImageGridModel *m_model;
		int m_targetWidth = 200;
		QSize m_tile {200, 250};
		QList<Action> m_actions;
		bool m_showRatings = true;
		QTimer *m_fadeTimer;
		QPersistentModelIndex m_hoverIndex;
		int m_hoverAction = -1;
		bool m_updatingGrid = false;
		int m_pressedAction = -1;
		QPersistentModelIndex m_pressedIndex;
};

#endif // IMAGE_GRID_H
