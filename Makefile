
CPPS=fs.cpp fs-dummyEntrypoint.cpp fs-manager.cpp fs-memory.cpp main.cpp dvuser.cpp
# $(wildcard *.cpp)
PWD=$(shell pwd)
NATIVEO=native/sys.o
DIVINE ?= /home/xstill/DiVinE/next

all : fs

fs : $(CPPS:.cpp=.o) libdivinert.o $(NATIVEO)
	ld $^ -o $@

libdivinert.o :
	divinecc --libraries-only --disable-vfs --standalone
	clang++ -c $(@:.o=.bc) -g

%.o : %.cpp
	divinecc --standalone --dont-link --disable-vfs $< -std=c++11 -I$(PWD) -I$(DIVINE)/bricks -g
	clang++ -c $(<:.cpp=.bc) -g

clean :
	rm -rf $(CPPS:.cpp=.bc) $(CPPS:.cpp=.o) mmap.o libdivinert.bc libdivinert.o fs

native/sys.o : native/sys.c
	clang -c $< -o $@ --sysroot=/try-to-avoid-using-system-includes -Inative -g

.PHONY : clean all
