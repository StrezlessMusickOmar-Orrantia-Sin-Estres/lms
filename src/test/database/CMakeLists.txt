
add_executable(test-database
	Artist.cpp
	Cluster.cpp
	DatabaseTest.cpp
	Release.cpp
	Track.cpp
	)

target_link_libraries(test-database PRIVATE
	lmsdatabase
	GTest::GTest
	)

if (NOT CMAKE_CROSSCOMPILING)
	gtest_discover_tests(test-database)
endif()

