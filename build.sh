#!/bin/bash
gcc -O2 terminal_video_player.c -o tvp $(pkg-config --cflags --libs libavformat libavcodec libswscale libswresample libavutil sdl2)
