#include "ui/nav-rail.h"
#include <QButtonGroup>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>


namespace
{
	QColor mix(const QColor &a, const QColor &b, double amount)
	{
		return QColor::fromRgbF(a.redF() * (1 - amount) + b.redF() * amount, a.greenF() * (1 - amount) + b.greenF() * amount, a.blueF() * (1 - amount) + b.blueF() * amount);
	}

	QString rgba(const QColor &color, int alpha)
	{
		return QStringLiteral("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha);
	}
}

NavRail::NavRail(QWidget *parent)
	: QWidget(parent), m_group(new QButtonGroup(this))
{
	setObjectName("navRail");
	setAttribute(Qt::WA_StyledBackground);
	setFixedWidth(84);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	m_layout = new QVBoxLayout(this);
	m_layout->setContentsMargins(8, 12, 8, 12);
	m_layout->setSpacing(4);
	m_group->setExclusive(true);
}

QIcon NavRail::icon(Icon icon, const QColor &color, int size)
{
	QPixmap pixmap(size, size);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	const double s = size;
	QPen pen(color, s / 14.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);
	const QRectF box(s * 0.16, s * 0.16, s * 0.68, s * 0.68);
	switch (icon) {
		case Discover: {
			painter.drawEllipse(box);
			QPainterPath needle;
			const QPointF c = box.center();
			const double r = box.width() * 0.3;
			needle.moveTo(c + QPointF(r * 0.75, -r * 0.75));
			needle.lineTo(c + QPointF(r * 0.22, r * 0.22));
			needle.lineTo(c + QPointF(-r * 0.75, r * 0.75));
			needle.lineTo(c + QPointF(-r * 0.22, -r * 0.22));
			needle.closeSubpath();
			painter.setBrush(color);
			painter.drawPath(needle);
			break;
		}
		case Search:
			painter.drawEllipse(QRectF(s * 0.18, s * 0.18, s * 0.46, s * 0.46));
			painter.drawLine(QPointF(s * 0.58, s * 0.58), QPointF(s * 0.82, s * 0.82));
			break;
		case Library:
			for (int row = 0; row < 2; ++row) {
				for (int column = 0; column < 2; ++column) {
					painter.drawRoundedRect(QRectF(s * (0.18 + column * 0.35), s * (0.18 + row * 0.35), s * 0.29, s * 0.29), s * 0.06, s * 0.06);
				}
			}
			break;
		case Downloads:
			painter.drawLine(QPointF(s * 0.5, s * 0.16), QPointF(s * 0.5, s * 0.6));
			painter.drawPolyline(QPolygonF({QPointF(s * 0.32, s * 0.43), QPointF(s * 0.5, s * 0.61), QPointF(s * 0.68, s * 0.43)}));
			painter.drawPolyline(QPolygonF({QPointF(s * 0.18, s * 0.66), QPointF(s * 0.18, s * 0.82), QPointF(s * 0.82, s * 0.82), QPointF(s * 0.82, s * 0.66)}));
			break;
		case Monitors:
			painter.drawEllipse(box);
			painter.drawLine(box.center(), box.center() + QPointF(0, -box.height() * 0.28));
			painter.drawLine(box.center(), box.center() + QPointF(box.width() * 0.2, box.height() * 0.12));
			break;
		case Following: {
			QPainterPath ribbon;
			ribbon.moveTo(s * 0.28, s * 0.16);
			ribbon.lineTo(s * 0.72, s * 0.16);
			ribbon.lineTo(s * 0.72, s * 0.84);
			ribbon.lineTo(s * 0.5, s * 0.66);
			ribbon.lineTo(s * 0.28, s * 0.84);
			ribbon.closeSubpath();
			painter.drawPath(ribbon);
			break;
		}
		case Log:
			for (int line = 0; line < 3; ++line) {
				const double y = s * (0.28 + line * 0.22);
				painter.drawLine(QPointF(s * 0.2, y), QPointF(s * (line == 2 ? 0.6 : 0.8), y));
			}
			break;
		case Settings: {
			const QPointF c(s / 2, s / 2);
			QPolygonF gear;
			for (int i = 0; i < 32; ++i) {
				const double angle = qDegreesToRadians(i * 360.0 / 32);
				const double radius = (i % 4 < 2) ? s * 0.36 : s * 0.27;
				gear.append(c + QPointF(std::cos(angle) * radius, std::sin(angle) * radius));
			}
			painter.drawPolygon(gear);
			painter.drawEllipse(c, s * 0.1, s * 0.1);
			break;
		}
	}
	painter.end();
	return QIcon(pixmap);
}

QToolButton *NavRail::makeButton(const QString &id, const QString &label, Icon icon, const QString &tooltip)
{
	auto *button = new QToolButton(this);
	button->setObjectName("nav_" + id);
	button->setText(label);
	button->setToolTip(tooltip);
	button->setAccessibleName(label);
	button->setAccessibleDescription(tooltip);
	button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
	button->setIconSize(QSize(26, 26));
	button->setCursor(Qt::PointingHandCursor);
	button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	button->setMinimumHeight(60);
	button->setFocusPolicy(Qt::TabFocus);
	m_buttons.insert(id, button);
	m_icons.insert(id, icon);
	return button;
}

QToolButton *NavRail::addPage(const QString &id, const QString &label, Icon icon, const QString &tooltip)
{
	auto *button = makeButton(id, label, icon, tooltip);
	button->setCheckable(true);
	m_group->addButton(button);
	m_layout->addWidget(button);
	connect(button, &QToolButton::clicked, this, [this, id]() { emit pageRequested(id); });
	refreshStyle();
	return button;
}

QToolButton *NavRail::addAction(const QString &id, const QString &label, Icon icon, const QString &tooltip)
{
	auto *button = makeButton(id, label, icon, tooltip);
	m_layout->addWidget(button);
	connect(button, &QToolButton::clicked, this, [this, id]() { emit actionRequested(id); });
	refreshStyle();
	return button;
}

void NavRail::addStretch()
{
	m_layout->addStretch(1);
}

void NavRail::setCurrent(const QString &id)
{
	if (auto *button = m_buttons.value(id)) {
		if (button->isCheckable()) {
			button->setChecked(true);
		}
	} else if (auto *checked = m_group->checkedButton()) {
		m_group->setExclusive(false);
		checked->setChecked(false);
		m_group->setExclusive(true);
	}
}

QString NavRail::current() const
{
	for (auto it = m_buttons.constBegin(); it != m_buttons.constEnd(); ++it) {
		if (it.value()->isChecked()) {
			return it.key();
		}
	}
	return {};
}

QToolButton *NavRail::button(const QString &id) const
{
	return m_buttons.value(id);
}

void NavRail::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refreshStyle();
}

void NavRail::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
		refreshStyle();
	}
}

void NavRail::refreshStyle()
{
	if (m_styling) {
		return;
	}
	m_styling = true;
	const QPalette colors = parentWidget() != nullptr ? parentWidget()->palette() : palette();
	const QColor window = colors.color(QPalette::Window);
	const QColor text = colors.color(QPalette::WindowText);
	const QColor accent = colors.color(QPalette::Highlight);
	const QColor muted = mix(text, window, 0.35);
	const QColor active = window.lightnessF() < 0.5 ? mix(accent, Qt::white, 0.35) : mix(accent, Qt::black, 0.15);
	setStyleSheet(QStringLiteral(
		"#navRail { background: %1; border: 0; border-right: 1px solid %2; }"
		"#navRail QToolButton { border: 0; border-radius: 12px; padding: 6px 0 4px 0; color: %3; background: transparent; font-size: 8pt; }"
		"#navRail QToolButton:hover { background: %4; color: %5; }"
		"#navRail QToolButton:checked { background: %6; color: %7; font-weight: 600; }"
		"#navRail QToolButton:focus { border: 1px solid %8; }"
	).arg(mix(window, text, 0.045).name(), rgba(text, 28), muted.name(), rgba(text, 22), text.name(), rgba(accent, 52), active.name(), rgba(accent, 160)));
	for (auto it = m_buttons.constBegin(); it != m_buttons.constEnd(); ++it) {
		QIcon combined;
		combined.addPixmap(icon(m_icons.value(it.key()), muted).pixmap(48, 48), QIcon::Normal, QIcon::Off);
		combined.addPixmap(icon(m_icons.value(it.key()), text).pixmap(48, 48), QIcon::Active, QIcon::Off);
		combined.addPixmap(icon(m_icons.value(it.key()), active).pixmap(48, 48), QIcon::Normal, QIcon::On);
		combined.addPixmap(icon(m_icons.value(it.key()), active).pixmap(48, 48), QIcon::Active, QIcon::On);
		it.value()->setIcon(combined);
	}
	m_styling = false;
}
