# Host test harness for the blindbox opened-box set (one open per badge
# per box, ever, except the replayable final booth).
#
# main/apps/opened_boxes.h is deliberately free of LVGL and ESP-IDF so it
# builds here with nothing but a C++17 compiler. Run this before touching
# the proximity trigger — a re-arm bug found in 200 ms beats the same bug
# found through a badge, a relay, and a server behind a multi-minute
# idf.py build.
#
#   make          full suite
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
APPS     := ../../main/apps

blindbox-test: main.cpp $(APPS)/opened_boxes.h
	$(CXX) $(CXXFLAGS) -I$(APPS) -o $@ main.cpp

.PHONY: check clean
check: blindbox-test
	./blindbox-test

clean:
	rm -f blindbox-test
