BINARY = icefeed

$(BINARY):
	g++ -std=c++17 -o icefeed main.cpp -lavformat -lavcodec -lavutil -lswresample -lswscale -lz -lm
