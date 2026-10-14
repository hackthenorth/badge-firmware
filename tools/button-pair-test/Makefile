CXX ?= c++
CXXFLAGS ?= -std=c++17 -g -Wall -Wextra -Werror -fsanitize=address,undefined

.PHONY: check clean
check: button-pair-test
	./button-pair-test

button-pair-test: main.cpp ../../main/apps/button_pair.h
	$(CXX) $(CXXFLAGS) -I../../main/apps main.cpp -o $@

clean:
	rm -f button-pair-test
