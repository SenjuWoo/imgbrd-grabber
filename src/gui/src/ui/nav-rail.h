#ifndef NAV_RAIL_H
#define NAV_RAIL_H

#include <QHash>
#include <QWidget>


class QButtonGroup;
class QToolButton;
class QVBoxLayout;

/** Vertical app navigation: one button per main page, plus actions at the bottom. */
class NavRail : public QWidget
{
	Q_OBJECT

	public:
		enum Icon { Discover, Search, Library, Downloads, Monitors, Following, Log, Settings };

		explicit NavRail(QWidget *parent = nullptr);
		QToolButton *addPage(const QString &id, const QString &label, Icon icon, const QString &tooltip);
		QToolButton *addAction(const QString &id, const QString &label, Icon icon, const QString &tooltip);
		void addStretch();
		void setCurrent(const QString &id);
		QString current() const;
		QToolButton *button(const QString &id) const;
		static QIcon icon(Icon icon, const QColor &color, int size = 48);

	signals:
		void pageRequested(const QString &id);
		void actionRequested(const QString &id);

	protected:
		void changeEvent(QEvent *event) override;
		void showEvent(QShowEvent *event) override;

	private:
		QToolButton *makeButton(const QString &id, const QString &label, Icon icon, const QString &tooltip);
		void refreshStyle();
		QVBoxLayout *m_layout;
		QButtonGroup *m_group;
		QHash<QString, QToolButton*> m_buttons;
		QHash<QString, Icon> m_icons;
		bool m_styling = false;
};

#endif // NAV_RAIL_H
