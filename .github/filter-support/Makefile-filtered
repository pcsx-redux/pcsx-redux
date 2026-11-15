BUILD ?= Release
DESTDIR ?= /usr/local

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
rwildcard=$(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))
CC_IS_CLANG := $(shell $(CC) --version | grep -q clang && echo true || echo false)

PACKAGES := zlib

ifeq ($(wildcard third_party/ELFIO/elfio/elfio.hpp),)
HAS_SUBMODULES = false
else
HAS_SUBMODULES = true
endif

CXXFLAGS += -std=c++2b
CPPFLAGS += `pkg-config --cflags $(PACKAGES)`
CPPFLAGS += -I.
CPPFLAGS += -Isrc
CPPFLAGS += -Ithird_party
CPPFLAGS += -Ithird_party/ELFIO
CPPFLAGS += -Ithird_party/fmt/include/
CPPFLAGS += -Ithird_party/magic_enum/include
CPPFLAGS += -Ithird_party/googletest/googletest/include
CPPFLAGS += -Ithird_party/PEGTL/include
CPPFLAGS += -Ithird_party/ucl -Ithird_party/ucl/include
CPPFLAGS += -g

CPPFLAGS_Release += -O3
CPPFLAGS_Debug += -O0
CPPFLAGS_Coverage += -O0
ifeq ($(CC_IS_CLANG),true)
    CPPFLAGS_Coverage += -fprofile-instr-generate -fcoverage-mapping
else
    CPPFLAGS_Coverage += -fprofile-arcs -ftest-coverage
endif
CPPFLAGS_asan += -O1 -fsanitize=address -fno-omit-frame-pointer
CPPFLAGS_ubsan += -O1 -fsanitize=undefined -fno-omit-frame-pointer
CPPFLAGS_lto += -O3 -flto=auto -fno-fat-lto-objects -flto-partition=one

ifeq ($(CC_IS_CLANG),true)
    CXXFLAGS += -fcoroutines-ts
else
    CXXFLAGS += -fcoroutines
endif

ifeq ($(UNAME_S),Darwin)
    CPPFLAGS += -mmacosx-version-min=10.15
    CPPFLAGS += -stdlib=libc++
endif

LDFLAGS += `pkg-config --libs $(PACKAGES)`

ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -lc++
    LDFLAGS += -mmacosx-version-min=10.15
else
    LDFLAGS += -lstdc++fs
endif

LDFLAGS += -g

ifeq ($(CC_IS_CLANG),true)
    LDFLAGS_Coverage += -fprofile-instr-generate -fcoverage-mapping
else
    LDFLAGS_Coverage += -fprofile-arcs -ftest-coverage
endif
LDFLAGS_asan += -fsanitize=address
LDFLAGS_ubsan += -fsanitize=undefined
LDFLAGS_lto += -O3 -flto=auto -flto-partition=one

CPPFLAGS += $(CPPFLAGS_$(BUILD)) -pthread
LDFLAGS += $(LDFLAGS_$(BUILD)) -pthread


LD := $(CXX)

SRCS := $(call rwildcard,src/,*.cc)
SRCS += third_party/fmt/src/os.cc third_party/fmt/src/format.cc
SRCS += $(wildcard third_party/cueparser/*.c)
SRCS += $(wildcard third_party/iec-60908b/*.c)
SRCS += third_party/ucl/src/n2e_99.c third_party/ucl/src/alloc.c

TOOLS = authoring exe2elf exe2exe exe2iso midi2psm midi2spd modconv ps1-packer psyq-obj-parser

##############################################################################

OBJECTS += $(patsubst %.c,%.o,$(filter %.c,$(SRCS)))
OBJECTS += $(patsubst %.cc,%.o,$(filter %.cc,$(SRCS)))
OBJECTS += $(patsubst %.cpp,%.o,$(filter %.cpp,$(SRCS)))
OBJECTS += $(patsubst %.mm,%.o,$(filter %.mm,$(SRCS)))

TESTS_SRC := $(call rwildcard,tests/,*.cc)
TESTS := $(patsubst %.cc,%,$(TESTS_SRC))

CP ?= cp
MKDIRP ?= mkdir -p

all: check_submodules dep tools

ifeq ($(HAS_SUBMODULES),true)
check_submodules:

else
check_submodules:
	@echo "You need to clone this repository recursively, in order to get its submodules."
	@false
endif

strip: all
	strip $(TOOLS)

install: all strip
	$(MKDIRP) $(DESTDIR)/bin
	$(CP) $(TOOLS) $(DESTDIR)/bin

%.o: %.c
	$(CC) -c -o $@ $< $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CFLAGS)

%.o: %.cc
	$(CXX) -c -o $@ $< $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CXXFLAGS)

%.o: %.cpp
	$(CXX) -c -o $@ $< $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CXXFLAGS)

%.o: %.mm
	$(CC) -c -o $@ $< $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CFLAGS)

%.dep: %.c
	$(CC) $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CFLAGS) -M -MT $(addsuffix .o, $(basename $@)) -MF $@ $<

%.dep: %.cc
	$(CXX) $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CXXFLAGS) -M -MT $(addsuffix .o, $(basename $@)) -MF $@ $<

%.dep: %.cpp
	$(CXX) $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CXXFLAGS) -M -MT $(addsuffix .o, $(basename $@)) -MF $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET) $(DEPS) gtest-all.o gtest_main.o

gtest-all.o: $(wildcard third_party/googletest/googletest/src/*.cc)
	$(CXX) -O3 -g $(CXXFLAGS) -Ithird_party/googletest/googletest -Ithird_party/googletest/googletest/include -c third_party/googletest/googletest/src/gtest-all.cc

gtest_main.o: third_party/googletest/googletest/src/gtest_main.cc
	$(CXX) -O3 -g $(CXXFLAGS) -Ithird_party/googletest/googletest -Ithird_party/googletest/googletest/include -c third_party/googletest/googletest/src/gtest_main.cc

gitclean:
	git clean -f -d -x
	git submodule foreach --recursive git clean -f -d -x

tests: $(foreach t,$(TESTS),$(t).o) $(NONMAIN_OBJECTS) gtest-all.o gtest_main.o
	$(LD) -o tests $(NONMAIN_OBJECTS) gtest-all.o gtest_main.o $(foreach t,$(TESTS),$(t).o) -Ithird_party/googletest/googletest/include $(LDFLAGS)

runtests: tests
	./tests

define TOOLDEF
$(1): $(OBJECTS) tools/$(1)/$(1).o
	$(LD) -o $(1) $(CPPFLAGS) $(CXXFLAGS) $(OBJECTS) tools/$(1)/$(1).o -static $(LDFLAGS)

endef

$(foreach tool,$(TOOLS),$(eval $(call TOOLDEF,$(tool))))

tools: $(TOOLS)

.PHONY: all dep clean gitclean runtests install strip tools

DEPS += $(patsubst %.c,%.dep,$(filter %.c,$(SRCS)))
DEPS := $(patsubst %.cc,%.dep,$(filter %.cc,$(SRCS)))
DEPS += $(patsubst %.cpp,%.dep,$(filter %.cpp,$(SRCS)))

dep: $(DEPS)

ifneq ($(MAKECMDGOALS), clean)
ifneq ($(MAKECMDGOALS), gitclean)
ifeq ($(HAS_SUBMODULES), true)
-include $(DEPS)
endif
endif
endif
