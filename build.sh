#!/bin/sh
CC=gcc
CFLAGS="-I/opt/vc/include -I/opt/vc/include/interface/vcos/pthreads"
LDFLAGS="-L/opt/vc/lib -lmmal -lmmal_core -lmmal_util -lmmal_vc_client"

$CC $CFLAGS $LDFLAGS -g -O0 -Wall -o mmal-renderer-test mmal-renderer-test.c
