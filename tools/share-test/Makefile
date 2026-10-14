# Host test harness for the App Share codec.
#
# app_share_proto.h and share_bundle.{h,cpp} are deliberately free of LVGL
# and ESP-IDF so they build here with nothing but a C++17 compiler. Run this
# before touching firmware — a codec bug found in 200 ms beats the same bug
# found through two badges behind a multi-minute idf.py build.
#
#   make          full suite
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SHARE    := ../../main/apps

share-test: main.cpp $(SHARE)/share_bundle.cpp $(SHARE)/share_bundle.h $(SHARE)/app_share_proto.h
	$(CXX) $(CXXFLAGS) -I$(SHARE) -o $@ main.cpp $(SHARE)/share_bundle.cpp

.PHONY: check clean
check: share-test
	./share-test
	python3 test-engine.py

clean:
	rm -f share-test
	rm -rf share-test.dSYM
