#include "analytics.h"
#include "catch.h"


TEST_CASE("Analytics", "[analytics]")
{
	SECTION("Usage reporting stays off whatever a profile asks for")
	{
		Analytics::getInstance().setEnabled(true);
		REQUIRE_FALSE(Analytics::getInstance().isEnabled());
	}
}
