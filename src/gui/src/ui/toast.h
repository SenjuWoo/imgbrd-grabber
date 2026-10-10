#ifndef TOAST_H
#define TOAST_H

#include <QLabel>


class QTimer;

/** Short, non-blocking confirmation shown at the bottom of a page. */
class Toast : public QLabel
{
	Q_OBJECT

	public:
		static Toast *show(QWidget *parent, const QString &text, int milliseconds = 2400);

	protected:
		bool eventFilter(QObject *watched, QEvent *event) override;

	private:
		explicit Toast(QWidget *parent);
		void place();
		QTimer *m_timer;
};

#endif // TOAST_H
