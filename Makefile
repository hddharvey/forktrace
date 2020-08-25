OUTPUTS = example tracer reaper

# Using C++20 support pretty much just for string::starts_with in command.cpp.
# Other than that, this uses quite a few features exclusive to C++14 / C++17.
CXX = g++
CXXFLAGS = -std=c++2a -g
LDFLAGS = -pthread -lncurses -lreadline
CC = gcc
CFLAGS = 

# Flags to generate dependency information
DEPFLAGS = -MMD -MP -MF"$(@:%.o=%.d)"

# Where we'll put intermediate files (like .o files and .d files)
BUILD_DIR = build

# Source files for tracer (.cpp only, we use gcc to generate header deps)
TRACER_SRCS = main.cpp \
       tracer.cpp \
       system.cpp \
       tracee.cpp \
       process.cpp \
       event.cpp \
       util.cpp \
       diagram.cpp \
       terminal.cpp \
       command.cpp

TRACER_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/tracer/%.o,$(TRACER_SRCS))

.PHONY: all
all: $(OUTPUTS)

.PHONY: clean
clean:
	rm -rf $(OUTPUTS)
	rm -rf $(BUILD_DIR)

###############################################################################
# tracer
###############################################################################

$(BUILD_DIR):
	mkdir -p $@
	mkdir -p $@/tracer

$(BUILD_DIR)/tracer/%.o: src/tracer/%.cpp | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) $(DEPFLAGS) $< -o $@

tracer: $(TRACER_OBJS)
	$(CXX) $^ $(LDFLAGS) -o $@

###############################################################################
# reaper
###############################################################################

reaper: src/reaper/*.*
	$(CC) $(CFLAGS) `ls src/reaper/*.c` -o $@

###############################################################################
# example
###############################################################################

example: src/example.c src/forktrace.h
	$(CC) $(CFLAGS) $^ -o $@

###############################################################################
# Header dependencies for tracer
###############################################################################

-include $(wildcard $(BUILD_DIR)/tracer/*.d)
