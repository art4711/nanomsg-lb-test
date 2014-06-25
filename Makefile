nlt: nlt.c
	cc -o nlt -O2 -Wall -Werror nlt.c -lrt -lnanomsg -lpthread -lanl
