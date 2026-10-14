# Host test harness for the chess rules engine.
#
# chess_rules.cpp is deliberately free of LVGL and ESP-IDF so it builds here
# with nothing but a C++17 compiler. Run this before touching firmware — a
# move-generation bug found in 200 ms beats the same bug found through a
# serial console behind a multi-minute idf.py build.
#
#   make          full suite (~10 s)
#   make quick    shallower depths (~1 s), for a fast edit loop
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
RULES    := ../../main/apps

perft: main.cpp $(RULES)/chess_rules.cpp $(RULES)/chess_rules.h
	$(CXX) $(CXXFLAGS) -I$(RULES) -o $@ main.cpp $(RULES)/chess_rules.cpp

.PHONY: check quick clean
check: perft
	./perft

quick: perft
	./perft --quick

clean:
	rm -f perft
