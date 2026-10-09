CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter

# Platform detection
ifeq ($(OS),Windows_NT)
    EXE := .exe
    LDFLAGS ?= -lwinmm -lgdi32 -static
    RUNNER_TARGET := gm14_runner$(EXE)
else
    EXE :=
    LDFLAGS ?= -lasound -lpthread
    RUNNER_TARGET :=
endif

TARGETS = gm14_host$(EXE) vm_host$(EXE) vm_tests$(EXE) $(RUNNER_TARGET)

all: $(TARGETS)

COMMON_OBJS = dw.o audio.o stb_vorbis_impl.o
VM_OBJS     = vm.o $(COMMON_OBJS)

dw.o: dw.cpp dw.hpp stb_image.h
	$(CXX) $(CXXFLAGS) -c dw.cpp -o $@

audio.o: audio.cpp audio.hpp stb_vorbis.c
	$(CXX) $(CXXFLAGS) -c audio.cpp -o $@

stb_vorbis_impl.o: stb_vorbis_impl.c stb_vorbis.c
	$(CXX) $(CXXFLAGS) -c stb_vorbis_impl.c -o $@

vm.o: vm.cpp vm.hpp dw.hpp audio.hpp
	$(CXX) $(CXXFLAGS) -c vm.cpp -o $@

gm14_host$(EXE): main_host.cpp dw.o
	$(CXX) $(CXXFLAGS) -o $@ main_host.cpp dw.o $(LDFLAGS)

vm_host$(EXE): vm_host.cpp $(VM_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ vm_host.cpp $(VM_OBJS) $(LDFLAGS)

vm_tests$(EXE): vm_tests.cpp $(VM_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ vm_tests.cpp $(VM_OBJS) $(LDFLAGS)

gm14_runner$(EXE): main_runner.cpp $(VM_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ main_runner.cpp $(VM_OBJS) $(LDFLAGS)

# Aliases without extension
gm14_host: gm14_host$(EXE)
vm_host: vm_host$(EXE)
vm_tests: vm_tests$(EXE)
gm14_runner: gm14_runner$(EXE)

clean:
	rm -f $(TARGETS) gm14_runner$(EXE) *.o out_room_*.png vm_frame.png test_intro_*.png
