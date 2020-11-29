# Gateway

Gateway is a dynamic tiling wayland compositor that puts the keyboard in focus. I created gateway because I wanted to try out certain policies for managing windows I hadn't seen before. Mainly I've tried to keep the code simple and fast while also trying out things like not allowing apps to grab focus. My knowlage of X11 are quite limited but I think this is the sort of thing you can't do without wayland. Since X11 apps can sniff all input into the system at all times a lot of them will declare themselves in focus or start reacting to keyboard presses when they shouldn't.

Gateway is being developed for use in InterstellarOS. Good xwayland support is a goal however certain X'isms like override-redirect makes this harder than it would be otherwise. There is currently basic wlr-layer-shell support so you can have a launcher or background.

## Building Gateway

To build gateway you need devel packages of the following on your system:
- wlroots
- wayland-protocols

to build run:
git clone https://github.com/SamHSmith/gateway.git
cd gateway
git submodule update --init
make

## Running Gateway

You can run Gateway with `./gateway`. In an existing Wayland or X11 session,
tinywl will open a Wayland or X11 window respectively to act as a virtual
display. You can then open Wayland windows by setting `WAYLAND_DISPLAY` to the
value shown in the logs. You can also run `./gateway` from a TTY.

You can specify `-s [cmd]` to run a command at startup, such as a terminal emulator.

- `Super+Escape`: Terminate the compositor

All the other keybindings are setup very weirdly because I use a customized keyboard layout based on dvorak. I will consolidate them in the future but for now you can view/change them by editing the handle_keybinding function on line 310 in src/gateway.c.

## Startup file

If you create the executable file $HOME/.config/gateway/startup.sh gateway will run it at startup. Useful for starting up swaybg to set the wallpaper.

## Limitations

Todo:
- tags (also know as "workspaces")
- HiDPI support
- Any kind of configuration, e.g. output layout
- Drag and Drop, used by filemanagers, even internally to one application.

