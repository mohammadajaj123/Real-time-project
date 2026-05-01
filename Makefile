CC      = gcc
CFLAGS  = -Wall -Wextra -g -fopenmp -I./include
LDFLAGS = -lm

TARGET  = furniture
SRCS    = src/main.c src/config.c src/member.c src/graphics.c

# ---- OpenGL detection -------------------------------------------------------
# Install dev headers with: sudo apt install freeglut3-dev libgl1-mesa-dev
GLUT_H  := $(shell ls /usr/include/GL/glut.h 2>/dev/null)

ifdef GLUT_H
    CFLAGS  += -DUSE_GRAPHICS
    LDFLAGS += -lGL -lGLU -lglut
    $(info OpenGL/GLUT found — building with graphics window)
else
    $(info OpenGL/GLUT headers not found — building with terminal display)
    $(info To enable graphics: sudo apt install freeglut3-dev libgl1-mesa-dev)
endif

# ---- Targets ----------------------------------------------------------------
.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Force terminal mode even if GLUT is available
term: CFLAGS := $(filter-out -DUSE_GRAPHICS, $(CFLAGS))
term: LDFLAGS := $(filter-out -lGL -lGLU -lglut, $(LDFLAGS))
term: $(SRCS)
	$(CC) $(CFLAGS) $^ -o $(TARGET)_term $(LDFLAGS)

run: all
	./$(TARGET) config.txt

clean:
	rm -f $(TARGET) $(TARGET)_term
	rm -f /tmp/rt_bwd_t*
