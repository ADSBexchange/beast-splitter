CXX=g++
CXXFLAGS=-std=c++11 -Wall -Werror -O -g
LIBS=-lboost_system -lpthread

all: testharness

testharness: modes_message.o modes_filter.o beast_settings.o beast_input.o beast_output.o testharness.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f *.o testharness
