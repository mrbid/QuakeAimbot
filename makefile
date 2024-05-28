all:
	gcc aim.c -Ofast -mfma -march=native -lX11 -lxdo -pthread -lm -o aim

deps:
	sudo apt install gcc xterm libx11-dev libxdo-dev

min:
	xterm -e ./aim 1 > /dev/null

clean:
	rm aim
