CPP=g++
CFLAGS=-I ../indexed_bzip2 -std=c++11 -Wall -Wextra -Wshadow -Wsuggest-override -O3 -DNDEBUG

bzcatpipe: bzcatpipe.o
	$(CPP) $^ -o $@

bzcatpipe.o: bzcatpipe.cpp ../indexed_bzip2/bzip2.h ../indexed_bzip2/BitReader.hpp ../indexed_bzip2/FileReader.hpp
	$(CPP) $(CFLAGS) -c $< -o $@

bzcat: bzcat.o
	$(CPP) $^ -o $@

bzcat.o: bzcat.cpp ../indexed_bzip2/bzip2.h ../indexed_bzip2/BitReader.hpp ../indexed_bzip2/FileReader.hpp
	$(CPP) $(CFLAGS) -c $< -o $@
