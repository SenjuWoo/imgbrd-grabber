#include "ui/toast.h"
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QTimer>


Toast::Toast(QWidget *parent)
	: QLabel(parent), m_timer(new QTimer(this))
{
	setObjectName("toast");
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setTextFormat(Qt::PlainText);
	setWordWrap(true);
	setAlignment(Qt::AlignCenter);
	// Fixed colors so the message stays readable on every theme.
	setStyleSheet("#toast { background: rgba(22, 22, 30, 235); color: #f4f4f8; border-radius: 16px; padding: 9px 18px; font-weight: 600; }");
	auto *effect = new QGraphicsOpacityEffect(this);
	setGraphicsEffect(effect);
	m_timer->setSingleShot(true);
	connect(m_timer, &QTimer::timeout, this, [this, effect]() {
		auto *fade = new QPropertyAnimation(effect, "opacity", this);
		fade->setDuration(260);
		fade->setStartValue(1.0);
		fade->setEndValue(0.0);
		connect(fade, &QPropertyAnimation::finished, this, &QObject::deleteLater);
		fade->start(QAbstractAnimation::DeleteWhenStopped);
	});
	parent->installEventFilter(this);
}

Toast *Toast::show(QWidget *parent, const QString &text, int milliseconds)
{
	for (auto *existing : parent->findChildren<Toast*>(QString(), Qt::FindDirectChildrenOnly)) {
		existing->deleteLater();
	}
	auto *toast = new Toast(parent);
	toast->setText(text);
	toast->setAccessibleName(text);
	toast->place();
	toast->QLabel::show();
	toast->raise();
	toast->m_timer->start(milliseconds);
	return toast;
}

void Toast::place()
{
	auto *parent = parentWidget();
	setMaximumWidth(std::max(160, parent->width() - 48));
	adjustSize();
	move((parent->width() - width()) / 2, parent->height() - height() - 28);
}

bool Toast::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == parentWidget() && event->type() == QEvent::Resize) {
		place();
	}
	return QLabel::eventFilter(watched, event);
}
