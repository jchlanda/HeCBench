#===============================================================================
# User Options
#===============================================================================

# Compiler can be set below, or via environment variable
CC        = hipcc
OPTIMIZE  = yes
DEBUG     = no

#===============================================================================
# Program name & source code list
#===============================================================================
program = mandelbrot

source = main.cu 

obj = $(source:.cu=.o)

#===============================================================================
# Sets Flags
#===============================================================================

# Standard Flags
CFLAGS := $(EXTRA_CFLAGS) -std=c++14 -Wall

# Linker Flags
LDFLAGS = 

# Debug Flags
ifeq ($(DEBUG),yes)
  CFLAGS += -g
  LDFLAGS  += -g
endif

# Optimization Flags
ifeq ($(OPTIMIZE),yes)
  CFLAGS += -O3
endif

#===============================================================================
# Targets to Build
#===============================================================================

$(program): $(obj) mandel.hpp common.hpp Makefile
	$(CC) $(CFLAGS) $(obj) -o $@ $(LDFLAGS)

%.o: %.cu mandel.hpp common.hpp Makefile
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(program) $(obj)

run: $(program)
	./$(program) 1000
