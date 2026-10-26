#!/bin/sh
# the terminal of the weston fallback on the 640x480 panel, maximized so it
# never runs off the screen
exec /usr/bin/weston-terminal --maximized "$@"