CXX ?= c++
CXXFLAGS ?= -std=c++17 -g -Wall -Wextra -Werror -fsanitize=address,undefined
APPS := ../../main/apps

.PHONY: check clean
check: button-bump-test
	./button-bump-test

button-bump-test: main.cpp $(APPS)/button_bump.h $(APPS)/button_pair.h $(APPS)/bump_proto.h $(APPS)/bump_proto.cpp
	$(CXX) $(CXXFLAGS) -I$(APPS) main.cpp $(APPS)/bump_proto.cpp -o $@

clean:
	rm -f button-bump-test
