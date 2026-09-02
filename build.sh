#!/bin/bash
gcc -O2 terminal_video_player.c -o terminal_video_player \
    $(pkg-config --cflags --libs libavformat libavcodec libswscale libavutil)
