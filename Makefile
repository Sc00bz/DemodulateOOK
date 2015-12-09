CC=g++
FLAGS=-Wall -O2

demodulate-ook: main.cpp
	$(CC) $(FLAGS) -o demodulate-ook main.cpp

clean:
	-rm demodulate-ook
