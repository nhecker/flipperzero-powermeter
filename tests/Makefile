CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -I..
BIN := test_pm_calc

$(BIN): test_pm_calc.c ../pm_calc.c ../pm_calc.h
	$(CC) $(CFLAGS) -o $@ test_pm_calc.c ../pm_calc.c

check: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: check clean
