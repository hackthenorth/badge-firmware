CXX ?= c++
CXXFLAGS ?= -std=c++17 -g -Wall -Wextra -Werror -fsanitize=address,undefined

.PHONY: check clean
check: blackjack-test
	./blackjack-test

blackjack-test: main.cpp ../../main/apps/blackjack_rules.h
	$(CXX) $(CXXFLAGS) -I../../main/apps main.cpp -o $@

clean:
	rm -f blackjack-test
