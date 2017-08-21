LDFLAGS = -pthread
CXXFLAGS = \
	-g -D_GNU_SOURCE \
	-I. -I./src -I./debug \
	--std=c++11 \

CXXFLAGS += -Wall
#CXXFLAGS += -O3

SKIPLIST = src/skiplist.o

TEST = tests/skiplist_test.o \
	   $(SKIPLIST)

ITR_TEST = \
	tests/mt_itr_write_erase_test.o \
	$(SKIPLIST)

PROGRAMS = \
	skiplist_test \
	itr_test \

all: $(PROGRAMS)

skiplist_test: $(TEST)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

itr_test: $(ITR_TEST)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(PROGRAMS) ./*.o ./*.so ./*/*.o ./*/*.so
