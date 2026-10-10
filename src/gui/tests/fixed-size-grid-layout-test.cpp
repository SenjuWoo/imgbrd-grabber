#include <QWidget>
#include "catch.h"
#include "ui/fixed-size-grid-layout.h"


TEST_CASE("Search grid keeps pictures close and centers the rows", "[search][grid]")
{
	QWidget parent;
	auto *layout = new FixedSizeGridLayout(&parent, 6, 6);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setFixedWidth(186);
	QList<QWidget*> tiles;
	for (int i = 0; i < 9; ++i) {
		auto *tile = new QWidget(&parent);
		tile->setFixedSize(186, 220);
		layout->addWidget(tile);
		tiles.append(tile);
	}
	tiles[3]->hide(); // A merged duplicate leaves no hole.
	layout->setGeometry(QRect(0, 0, 1500, 1000));

	// 7 columns fit 1500 px; before, the 198 spare pixels became 33 px gaps.
	const int gap = tiles[1]->geometry().left() - tiles[0]->geometry().right() - 1;
	REQUIRE(gap >= 6);
	REQUIRE(gap <= 12);
	REQUIRE(tiles[4]->geometry().left() == tiles[2]->geometry().right() + 1 + gap);
	const int leftMargin = tiles[0]->geometry().left();
	const int rightMargin = 1500 - (tiles[7]->geometry().right() + 1);
	REQUIRE(std::abs(leftMargin - rightMargin) <= 1);
	REQUIRE(tiles[8]->geometry().left() == leftMargin); // Second row starts at the same indent.
	REQUIRE(tiles[8]->geometry().top() == 226);
}
