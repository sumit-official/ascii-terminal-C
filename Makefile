CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

ascii-city: ascii-city.c
	$(CC) $(CFLAGS) -o $@ $< -lm

run: ascii-city
	./ascii-city

clean:
	rm -f ascii-city

.PHONY: run clean
