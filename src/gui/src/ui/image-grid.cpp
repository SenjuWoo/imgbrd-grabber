#include "ui/image-grid.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSet>
#include <QStyle>
#include <QTimer>
#include <QtMath>


namespace
{
	constexpr int Gap = 8;
	constexpr int Radius = 10;
	constexpr int FadeMs = 220;
	constexpr double TileRatio = 1.25;

	const QColor LikeColor(255, 92, 138);
	const QColor FavoriteColor(255, 200, 69);

	QPainterPath heart(const QRectF &r)
	{
		const double x = r.x(), y = r.y(), w = r.width(), h = r.height();
		QPainterPath path;
		path.moveTo(x + w / 2, y + h * 0.9);
		path.cubicTo(x + w * 0.1, y + h * 0.62, x - w * 0.02, y + h * 0.3, x + w * 0.22, y + h * 0.14);
		path.cubicTo(x + w * 0.36, y + h * 0.04, x + w * 0.47, y + h * 0.12, x + w / 2, y + h * 0.26);
		path.cubicTo(x + w * 0.53, y + h * 0.12, x + w * 0.64, y + h * 0.04, x + w * 0.78, y + h * 0.14);
		path.cubicTo(x + w * 1.02, y + h * 0.3, x + w * 0.9, y + h * 0.62, x + w / 2, y + h * 0.9);
		path.closeSubpath();
		return path;
	}

	QPainterPath star(const QRectF &r)
	{
		QPainterPath path;
		const QPointF center = r.center() + QPointF(0, r.height() * 0.04);
		const double outer = std::min(r.width(), r.height()) / 2.0;
		for (int i = 0; i < 10; ++i) {
			const double radius = i % 2 == 0 ? outer : outer * 0.45;
			const double angle = qDegreesToRadians(-90.0 + i * 36.0);
			const QPointF point = center + QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
			i == 0 ? path.moveTo(point) : path.lineTo(point);
		}
		path.closeSubpath();
		return path;
	}

	void drawIcon(QPainter *painter, ImageGridView::Action action, const QRectF &r, bool active)
	{
		const QRectF icon = r.adjusted(r.width() * 0.27, r.height() * 0.27, -r.width() * 0.27, -r.height() * 0.27);
		QPen pen(Qt::white, std::max(1.6, r.width() / 16.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		painter->setPen(pen);
		painter->setBrush(Qt::NoBrush);
		switch (action) {
			case ImageGridView::Like:
				if (active) {
					painter->setPen(Qt::NoPen);
					painter->setBrush(LikeColor);
				}
				painter->drawPath(heart(icon));
				break;
			case ImageGridView::Favorite:
				if (active) {
					painter->setPen(Qt::NoPen);
					painter->setBrush(FavoriteColor);
				}
				painter->drawPath(star(icon));
				break;
			case ImageGridView::Download: {
				const double cx = icon.center().x();
				painter->drawLine(QPointF(cx, icon.top()), QPointF(cx, icon.top() + icon.height() * 0.62));
				painter->drawPolyline(QPolygonF({QPointF(cx - icon.width() * 0.28, icon.top() + icon.height() * 0.36), QPointF(cx, icon.top() + icon.height() * 0.64), QPointF(cx + icon.width() * 0.28, icon.top() + icon.height() * 0.36)}));
				painter->drawPolyline(QPolygonF({QPointF(icon.left(), icon.bottom() - icon.height() * 0.18), QPointF(icon.left(), icon.bottom()), QPointF(icon.right(), icon.bottom()), QPointF(icon.right(), icon.bottom() - icon.height() * 0.18)}));
				break;
			}
			case ImageGridView::Hide: {
				const QRectF cross = icon.adjusted(icon.width() * 0.1, icon.height() * 0.1, -icon.width() * 0.1, -icon.height() * 0.1);
				painter->drawLine(cross.topLeft(), cross.bottomRight());
				painter->drawLine(cross.topRight(), cross.bottomLeft());
				break;
			}
		}
	}
}


int ImageGridModel::rowCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : int(m_items.size());
}

QVariant ImageGridModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || index.row() >= m_items.size()) {
		return {};
	}
	const auto &item = m_items[index.row()];
	switch (role) {
		case KeyRole: return item.key;
		case LikedRole: return item.liked;
		case FavoriteRole: return item.favorite;
		case BadgeRole: return item.badge;
		case ShownAtRole: return item.shownAt;
		case Qt::ToolTipRole:
		case Qt::AccessibleDescriptionRole: return item.tooltip;
		case Qt::AccessibleTextRole: return item.title;
		case PixmapRole: {
			if (!item.pixmap.isNull() || item.encoded.isEmpty()) {
				return item.pixmap;
			}
			if (const QPixmap *cached = m_decoded.object(item.key)) {
				return *cached;
			}
			QPixmap decoded;
			decoded.loadFromData(item.encoded);
			if (!decoded.isNull()) {
				m_decoded.insert(item.key, new QPixmap(decoded), std::max<qint64>(1, qint64(decoded.width()) * decoded.height() * 4 / 1024));
			}
			return decoded;
		}
		default: return {};
	}
}

void ImageGridModel::reindex(int from)
{
	for (int i = from; i < m_items.size(); ++i) {
		m_rows.insert(m_items[i].key, i);
	}
}

void ImageGridModel::setItems(const QList<ImageGridItem> &items)
{
	beginResetModel();
	m_items.clear();
	m_rows.clear();
	m_decoded.clear();
	for (const auto &item : items) {
		if (!item.key.isEmpty() && !m_rows.contains(item.key)) {
			m_rows.insert(item.key, int(m_items.size()));
			m_items.append(item);
		}
	}
	endResetModel();
}

void ImageGridModel::append(const QList<ImageGridItem> &items)
{
	QList<ImageGridItem> fresh;
	QSet<QString> keys;
	for (const auto &item : items) {
		if (!item.key.isEmpty() && !m_rows.contains(item.key) && !keys.contains(item.key)) {
			keys.insert(item.key);
			fresh.append(item);
		}
	}
	if (fresh.isEmpty()) {
		return;
	}
	const int first = int(m_items.size());
	beginInsertRows({}, first, first + int(fresh.size()) - 1);
	m_items.append(fresh);
	reindex(first);
	endInsertRows();
}

void ImageGridModel::clear()
{
	setItems({});
}

bool ImageGridModel::contains(const QString &key) const { return m_rows.contains(key); }
int ImageGridModel::row(const QString &key) const { return m_rows.value(key, -1); }
const ImageGridItem &ImageGridModel::item(int row) const { return m_items[row]; }

QStringList ImageGridModel::keys() const
{
	QStringList result;
	result.reserve(m_items.size());
	for (const auto &item : m_items) {
		result.append(item.key);
	}
	return result;
}

void ImageGridModel::setPixmap(const QString &key, const QPixmap &pixmap)
{
	const int position = row(key);
	if (position < 0) {
		return;
	}
	m_items[position].pixmap = pixmap;
	m_items[position].shownAt = QDateTime::currentMSecsSinceEpoch();
	m_decoded.remove(key);
	emit dataChanged(index(position), index(position));
}

void ImageGridModel::setRating(const QString &key, bool liked, bool favorite)
{
	const int position = row(key);
	if (position < 0 || (m_items[position].liked == liked && m_items[position].favorite == favorite)) {
		return;
	}
	m_items[position].liked = liked;
	m_items[position].favorite = favorite;
	emit dataChanged(index(position), index(position), {LikedRole, FavoriteRole});
}

void ImageGridModel::remove(const QString &key)
{
	const int position = row(key);
	if (position < 0) {
		return;
	}
	beginRemoveRows({}, position, position);
	m_items.removeAt(position);
	m_rows.remove(key);
	m_decoded.remove(key);
	reindex(position);
	endRemoveRows();
}


ImageGridDelegate::ImageGridDelegate(ImageGridView *view)
	: QStyledItemDelegate(view), m_view(view)
{}

QSize ImageGridDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	Q_UNUSED(option)
	Q_UNUSED(index)
	return m_view->tileSize();
}

void ImageGridDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	painter->save();
	painter->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
	const QRect tile = QRect(option.rect.topLeft(), m_view->tileSize());
	QPainterPath shape;
	shape.addRoundedRect(QRectF(tile).adjusted(0.5, 0.5, -0.5, -0.5), Radius, Radius);
	const QPalette &palette = option.palette;
	const bool selected = option.state & QStyle::State_Selected;
	const bool hovered = option.state & QStyle::State_MouseOver;

	// Placeholder that blends with any theme.
	QColor placeholder = palette.color(QPalette::Base);
	const QColor text = palette.color(QPalette::Text);
	placeholder = QColor((placeholder.red() * 9 + text.red()) / 10, (placeholder.green() * 9 + text.green()) / 10, (placeholder.blue() * 9 + text.blue()) / 10);
	painter->fillPath(shape, placeholder);

	const QPixmap pixmap = index.data(ImageGridModel::PixmapRole).value<QPixmap>();
	if (!pixmap.isNull()) {
		const qreal ratio = painter->device()->devicePixelRatioF();
		const QSize target = tile.size() * ratio;
		const QString cacheKey = index.data(ImageGridModel::KeyRole).toString() + '@' + QString::number(target.width()) + 'x' + QString::number(target.height()) + ':' + QString::number(pixmap.cacheKey());
		QPixmap *scaled = m_scaled.object(cacheKey);
		if (scaled == nullptr) {
			const QPixmap cover = pixmap.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
			// Portrait art usually keeps faces near the top; bias the crop upwards.
			const int x = (cover.width() - target.width()) / 2;
			const int y = int((cover.height() - target.height()) * 0.2);
			scaled = new QPixmap(cover.copy(x, y, target.width(), target.height()));
			scaled->setDevicePixelRatio(ratio);
			m_scaled.insert(cacheKey, scaled, std::max<qint64>(1, qint64(target.width()) * target.height() * 4 / 1024));
		}
		const qint64 shownAt = index.data(ImageGridModel::ShownAtRole).toLongLong();
		const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - shownAt;
		const double opacity = shownAt <= 0 ? 1.0 : std::clamp(double(elapsed) / FadeMs, 0.0, 1.0);
		painter->save();
		painter->setClipPath(shape);
		painter->setOpacity(opacity);
		painter->drawPixmap(tile.topLeft(), *scaled);
		painter->restore();
	}

	if (hovered && !selected) {
		painter->fillPath(shape, QColor(255, 255, 255, 22));
	}

	// Media badge (GIF, video, page count).
	const QString badge = index.data(ImageGridModel::BadgeRole).toString();
	if (!badge.isEmpty()) {
		QFont font = option.font;
		font.setBold(true);
		font.setPointSizeF(std::max(7.0, font.pointSizeF() * 0.85));
		painter->setFont(font);
		const QFontMetrics metrics(font);
		const QRectF pill(tile.right() - metrics.horizontalAdvance(badge) - 18, tile.top() + 8, metrics.horizontalAdvance(badge) + 12, metrics.height() + 4);
		painter->setPen(Qt::NoPen);
		painter->setBrush(QColor(0, 0, 0, 150));
		painter->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
		painter->setPen(Qt::white);
		painter->drawText(pill, Qt::AlignCenter, badge);
	}

	const bool liked = index.data(ImageGridModel::LikedRole).toBool();
	const bool favorite = index.data(ImageGridModel::FavoriteRole).toBool();
	if (selected) {
		const auto &actions = m_view->actions();
		if (!actions.isEmpty()) {
			painter->save();
			painter->setClipPath(shape);
			const QRect first = m_view->actionRect(tile, 0);
			QLinearGradient shade(0, first.top() - 28, 0, tile.bottom());
			shade.setColorAt(0, QColor(0, 0, 0, 0));
			shade.setColorAt(1, QColor(0, 0, 0, 170));
			painter->fillRect(QRect(tile.left(), first.top() - 28, tile.width(), tile.bottom() - first.top() + 29), shade);
			painter->restore();
			const int hover = m_view->hoveredAction(index);
			for (int i = 0; i < actions.size(); ++i) {
				const QRectF button = m_view->actionRect(tile, i);
				painter->setPen(Qt::NoPen);
				painter->setBrush(QColor(255, 255, 255, hover == i ? 70 : 34));
				painter->drawEllipse(button);
				const bool active = (actions[i] == ImageGridView::Like && liked) || (actions[i] == ImageGridView::Favorite && favorite);
				drawIcon(painter, actions[i], button, active);
			}
		}
		QPen ring(palette.color(QPalette::Highlight), 3);
		painter->setPen(ring);
		painter->setBrush(Qt::NoBrush);
		painter->drawRoundedRect(QRectF(tile).adjusted(1.5, 1.5, -1.5, -1.5), Radius - 1, Radius - 1);
	} else if ((liked || favorite) && m_view->showRatings()) {
		const QRectF mark(tile.left() + 8, tile.bottom() - 31, 24, 24);
		painter->setPen(Qt::NoPen);
		painter->setBrush(QColor(0, 0, 0, 140));
		painter->drawEllipse(mark);
		painter->setBrush(favorite ? FavoriteColor : LikeColor);
		const QRectF icon = mark.adjusted(5.5, 5.5, -5.5, -5.5);
		painter->drawPath(favorite ? star(icon) : heart(icon));
	}
	if ((option.state & QStyle::State_HasFocus) && !selected && m_view->hasFocus()) {
		painter->setPen(QPen(palette.color(QPalette::Highlight), 1.5, Qt::DashLine));
		painter->setBrush(Qt::NoBrush);
		painter->drawRoundedRect(QRectF(tile).adjusted(1, 1, -1, -1), Radius, Radius);
	}
	painter->restore();
}


ImageGridView::ImageGridView(QWidget *parent)
	: QListView(parent), m_model(new ImageGridModel(this)), m_fadeTimer(new QTimer(this))
{
	setObjectName("imageGrid");
	setModel(m_model);
	setItemDelegate(new ImageGridDelegate(this));
	setViewMode(QListView::IconMode);
	setMovement(QListView::Static);
	setResizeMode(QListView::Adjust);
	setWrapping(true);
	setUniformItemSizes(true);
	// Single pass: uniform tiles lay out fast, and the scroll range is always complete.
	setLayoutMode(QListView::SinglePass);
	setSpacing(0);
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setSelectionRectVisible(true);
	setEditTriggers(QAbstractItemView::NoEditTriggers);
	setDragEnabled(false);
	setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setFrameShape(QFrame::NoFrame);
	setMouseTracking(true);
	viewport()->setAttribute(Qt::WA_Hover);
	verticalScrollBar()->setSingleStep(48);
	m_fadeTimer->setInterval(16);
	connect(m_fadeTimer, &QTimer::timeout, this, [this]() {
		viewport()->update();
		if (m_fadeTimer->property("until").toLongLong() < QDateTime::currentMSecsSinceEpoch()) {
			m_fadeTimer->stop();
		}
	});
	connect(m_model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
		if (roles.isEmpty()) {
			startFade();
		}
	});
	connect(m_model, &QAbstractItemModel::rowsInserted, this, [this]() { QTimer::singleShot(0, this, &ImageGridView::checkNearEnd); });
	connect(m_model, &QAbstractItemModel::modelReset, this, [this]() { QTimer::singleShot(0, this, &ImageGridView::checkNearEnd); });
	connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &ImageGridView::checkNearEnd);
	connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &ImageGridView::checkNearEnd);
	m_actions = {Like, Favorite, Download};
	updateGrid();
}

ImageGridModel *ImageGridView::gridModel() const { return m_model; }
QSize ImageGridView::tileSize() const { return m_tile; }
const QList<ImageGridView::Action> &ImageGridView::actions() const { return m_actions; }
bool ImageGridView::showRatings() const { return m_showRatings; }

int ImageGridView::densityWidth(int density)
{
	return density <= 0 ? 150 : density == 1 ? 210 : 290;
}

void ImageGridView::setDensity(int density)
{
	setTargetWidth(densityWidth(qBound(0, density, 2)));
}

void ImageGridView::setTargetWidth(int width)
{
	m_targetWidth = qBound(80, width, 600);
	updateGrid();
}

void ImageGridView::setActions(const QList<Action> &actions)
{
	m_actions = actions;
	viewport()->update();
}

void ImageGridView::setShowRatings(bool show)
{
	m_showRatings = show;
	viewport()->update();
}

void ImageGridView::startFade()
{
	m_fadeTimer->setProperty("until", QDateTime::currentMSecsSinceEpoch() + FadeMs + 40);
	if (!m_fadeTimer->isActive()) {
		m_fadeTimer->start();
	}
}

void ImageGridView::updateGrid()
{
	// Mirror QListView's own wrapping width: it always reserves the scrollbar extent, and wraps one pixel early.
	const int extent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar());
	const int available = std::max(1, maximumViewportSize().width() - extent - 2);
	const int columns = std::max(1, (available + Gap) / (m_targetWidth + Gap));
	const int cell = available / columns;
	const int tileWidth = std::max(40, cell - Gap);
	const QSize tile(tileWidth, int(std::round(tileWidth * TileRatio)));
	if (!m_updatingGrid && (tile != m_tile || gridSize() != QSize(cell, tile.height() + Gap))) {
		m_updatingGrid = true;
		m_tile = tile;
		setGridSize(QSize(cell, tile.height() + Gap));
		doItemsLayout();
		m_updatingGrid = false;
	}
}

void ImageGridView::resizeEvent(QResizeEvent *event)
{
	QListView::resizeEvent(event);
	updateGrid();
}

void ImageGridView::updateGeometries()
{
	QListView::updateGeometries();
	// The viewport also changes width when the scrollbar appears or the style changes.
	updateGrid();
}

void ImageGridView::checkNearEnd()
{
	if (m_model->rowCount() == 0) {
		return;
	}
	const auto *bar = verticalScrollBar();
	if (bar->maximum() - bar->value() < viewport()->height() * 1.5) {
		emit nearEnd();
	}
}

QRect ImageGridView::actionRect(const QRect &tile, int position) const
{
	const int count = std::max(1, int(m_actions.size()));
	int diameter = qBound(26, tile.width() / 5, 38);
	const int spacing = 8;
	while (diameter > 18 && count * diameter + (count - 1) * spacing > tile.width() - 12) {
		--diameter;
	}
	const int total = count * diameter + (count - 1) * spacing;
	const int left = tile.left() + (tile.width() - total) / 2;
	return {left + position * (diameter + spacing), tile.bottom() - diameter - 10, diameter, diameter};
}

int ImageGridView::actionAt(const QModelIndex &index, const QPoint &position) const
{
	if (!index.isValid() || !selectionModel()->isSelected(index)) {
		return -1;
	}
	const QRect tile(visualRect(index).topLeft(), m_tile);
	for (int i = 0; i < m_actions.size(); ++i) {
		const QRect button = actionRect(tile, i);
		const QPoint offset = position - button.center();
		if (offset.x() * offset.x() + offset.y() * offset.y() <= (button.width() / 2 + 2) * (button.width() / 2 + 2)) {
			return i;
		}
	}
	return -1;
}

int ImageGridView::hoveredAction(const QModelIndex &index) const
{
	return index == m_hoverIndex ? m_hoverAction : -1;
}

QStringList ImageGridView::selectedKeys() const
{
	QModelIndexList indexes = selectionModel()->selectedIndexes();
	std::sort(indexes.begin(), indexes.end(), [](const QModelIndex &left, const QModelIndex &right) { return left.row() < right.row(); });
	QStringList keys;
	for (const auto &index : indexes) {
		keys.append(index.data(ImageGridModel::KeyRole).toString());
	}
	return keys;
}

QString ImageGridView::currentKey() const
{
	return currentIndex().data(ImageGridModel::KeyRole).toString();
}

void ImageGridView::mousePressEvent(QMouseEvent *event)
{
	const QModelIndex index = indexAt(event->position().toPoint());
	const int action = event->button() == Qt::LeftButton ? actionAt(index, event->position().toPoint()) : -1;
	if (action >= 0) {
		m_pressedAction = action;
		m_pressedIndex = index;
		event->accept();
		return;
	}
	m_pressedAction = -1;
	QListView::mousePressEvent(event);
}

void ImageGridView::mouseReleaseEvent(QMouseEvent *event)
{
	if (m_pressedAction >= 0) {
		const QModelIndex index = indexAt(event->position().toPoint());
		const int action = actionAt(index, event->position().toPoint());
		const int pressed = m_pressedAction;
		m_pressedAction = -1;
		if (index == m_pressedIndex && action == pressed && pressed < m_actions.size()) {
			emit actionTriggered(m_actions[pressed], {index.data(ImageGridModel::KeyRole).toString()});
		}
		event->accept();
		return;
	}
	QListView::mouseReleaseEvent(event);
}

void ImageGridView::mouseDoubleClickEvent(QMouseEvent *event)
{
	const QModelIndex index = indexAt(event->position().toPoint());
	if (index.isValid() && actionAt(index, event->position().toPoint()) < 0 && event->button() == Qt::LeftButton) {
		emit openRequested(index.data(ImageGridModel::KeyRole).toString());
		event->accept();
		return;
	}
	event->accept();
}

void ImageGridView::mouseMoveEvent(QMouseEvent *event)
{
	const QModelIndex index = indexAt(event->position().toPoint());
	const int action = actionAt(index, event->position().toPoint());
	if (QPersistentModelIndex(index) != m_hoverIndex || action != m_hoverAction) {
		if (m_hoverIndex.isValid()) {
			viewport()->update(visualRect(m_hoverIndex));
		}
		m_hoverIndex = index;
		m_hoverAction = action;
		viewport()->setCursor(action >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
		if (index.isValid()) {
			viewport()->update(visualRect(index));
		}
	}
	QListView::mouseMoveEvent(event);
}

void ImageGridView::leaveEvent(QEvent *event)
{
	if (m_hoverIndex.isValid()) {
		viewport()->update(visualRect(m_hoverIndex));
	}
	m_hoverIndex = QPersistentModelIndex();
	m_hoverAction = -1;
	QListView::leaveEvent(event);
}

void ImageGridView::keyPressEvent(QKeyEvent *event)
{
	const QStringList keys = selectedKeys();
	const bool plain = event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier;
	if (plain && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentIndex().isValid()) {
		emit openRequested(currentKey());
		return;
	}
	if (plain && !keys.isEmpty()) {
		const QHash<int, Action> shortcuts {{Qt::Key_L, Like}, {Qt::Key_F, Favorite}, {Qt::Key_D, Download}, {Qt::Key_X, Hide}, {Qt::Key_Delete, Hide}};
		const auto it = shortcuts.constFind(event->key());
		if (it != shortcuts.cend() && m_actions.contains(*it)) {
			emit actionTriggered(*it, keys);
			return;
		}
	}
	QListView::keyPressEvent(event);
}

void ImageGridView::contextMenuEvent(QContextMenuEvent *event)
{
	const QModelIndex index = indexAt(event->pos());
	if (!index.isValid()) {
		return;
	}
	if (!selectionModel()->isSelected(index)) {
		selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
		selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
	}
	emit contextMenuRequested(selectedKeys(), event->globalPos());
}
