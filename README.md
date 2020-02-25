# Cage-Present: Do multimedia presentations the Unix way!

Cage-Present is a [wlroots](https://github.com/swaywm/wlroots/)-based Wayland
compositor for Linux. It is a tweaked version of the
[Cage](https://github.com/Hjdskes/cage) compositor by [Jente
Hidskes](https://www.hjdskes.nl/), which is itself based on the annotated source
of tinywl and rootston by the authors of [Sway](https://github.com/swaywm/sway),
so most of the credit should go to these people. Cage-Present lets you show off
multimedia presentations that are backed by all the native software on your
computer.

## What does Cage-Present do?

### The Problem:

You want to give a presentation that includes all sorts of multimedia, like
regular pdfs, but also videos, sounds, the whole shebang.

### Solution A:

* Spend millions creating a proprietary presentation document format and editor
  that can embed and display a zillion different types of media. 

### Solution B:

* Let your OS's already existing document and media viewers do all the work, and
  wrap the result up in a single maximisable window as a Wayland compositor.

## How to use

Suppose you want to present something that consists of a multi-page beamer pdf,
and an mp4 video, before jumping into an interactive terminal session. To do
that, just write a simple script:

`cmds.sh`:

```
xdg-open slides.pdf
xdg-open video.mp4
$TERMINAL
```

Then just run `cage-present cmds.sh`, and cage-present will spawn a new Wayland
compositor with `slides.pdf` maximised inside. You can then advance from the
slides to `diagram.pdf` by pressing ALT+Right (by default). Press ALT+Escape to
exit the compositor. The compositor will automatically exit after advancing past
the final command.

The real power of a modular, agnostic approach like this is that you can
leverage all of the software on your computer to display any media format at
your disposal, without having to wait for someone like Microsoft to decide that
it's a good idea. You can even seamlessly transition into any kind of
interactive demonstration without having to manually un-fullscreen the
presentation and open another window. Of course, the obvious caveat of running
arbitrary software is that the only safe option is to run your own presentations
on your own computer.

## Building

Dependencies: wayland, wlroots, xkbcommon

Building requires the [meson build system](https://mesonbuild.com/)


```
$ meson build

$ ninja -C build
```

To run Xorg applications inside Cage-Present, use the command `meson build
--Dxwayland=true`, and have a version of wlroots with xwayland support.

Run with `./build/cage-present path/to/cmds.sh`
