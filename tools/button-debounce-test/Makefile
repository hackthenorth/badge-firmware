CXX ?= c++
CXXFLAGS ?= -std=c++17 -g -Wall -Wextra -Werror -fsanitize=address,undefined

.PHONY: check clean

check: button-debounce-test
	./button-debounce-test

button-debounce-test: main.cpp ../../main/hal/button_debouncer.h
	$(CXX) $(CXXFLAGS) -I../../main/hal main.cpp -o $@

clean:
	rm -f button-debounce-test
