#ifndef TOAST_H
#define TOAST_H

#include <QLabel>
#include <functional>


class QTimer;

/** Short, non-blocking confirmation shown at the bottom of a page. */
class Toast : public QLabel
{
	Q_OBJECT

	public:
		static Toast *show(QWidget *parent, const QString &text, int milliseconds = 2400);
		/** Toast with one clickable action, such as Undo, that runs `action` and closes the toast. */
		static Toast *show(QWidget *parent, const QString &text, const QString &actionText, const std::function<void()> &action, int milliseconds = 6000);

	protected:
		bool eventFilter(QObject *watched, QEvent *event) override;

	private:
		explicit Toast(QWidget *parent);
		void place();
		QTimer *m_timer;
};

#endif // TOAST_H
