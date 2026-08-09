CC = cc
CFLAGS = -std=c11 -Wall -Wextra -O2
TARGET = retirement_sim
SRCS = main.c config.c tax.c rrif.c sim.c monte_carlo.c

$(TARGET): $(SRCS) retirement_sim.h dateutil.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) -lm

clean:
	rm -f $(TARGET) *.o

.PHONY: clean
