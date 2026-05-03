CC      = gcc
CFLAGS  = -Wall -Wextra -g -fopenmp -I./include
LDFLAGS = -lGL -lGLU -lglut -lm

TARGET  = furniture
SRCS    = src/main.c src/config.c src/member.c src/graphics.c

# Install dev headers with: sudo apt install freeglut3-dev libgl1-mesa-dev

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: all
	./$(TARGET) config.txt

clean:
	rm -f $(TARGET)
	rm -f /tmp/rt_bwd_t*
