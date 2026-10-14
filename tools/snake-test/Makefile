CXX ?= c++
CXXFLAGS ?= -std=c++17 -g -Wall -Wextra -Werror -fsanitize=address,undefined

.PHONY: check clean
check: snake-test
	./snake-test

snake-test: main.cpp ../../main/apps/snake_body.h
	$(CXX) $(CXXFLAGS) -I../../main/apps main.cpp -o $@

clean:
	rm -f snake-test
