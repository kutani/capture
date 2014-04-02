capture
=======

# What It Is

I wrote this one day when I felt like experimenting with capturing X11
window framebuffer data (for doing screencasts and such). It creates a
shared memory area for reading a given window's framebuffer and passes
passes data to an output routine, which writes to stdout.

It is multithreaded and buffered to try to prevent underruns, but
frankly it can still have some issues when piping to eg ffmpeg, which
is what I designed it for.

I got as far as getting it working (with sketchy framerate handling)
before I realized I could just modify ffmpeg's X11grab driver to do
what I needed.

Still, this (very) ugly bit of code might be educational.

# Usage

The build executable can be run one of three ways:

* No arguments: Reads from the root window, assuming width and height
  of 1920x1080
* Two arguments: Reads from the root window, with the arguments being
  width and height to capture, in order ('./capture 800 600')
* Three arguments: X11 window ID, width, and height, in order
