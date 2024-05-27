# Framebuffer Graphics

## Quoted from richinfante

**This is a modified version of a really old project I build back in high school. I'm still in progress of making it more modern and fast, and at the moment, it compiles, but is most likely very buggy**

At it's heart, this project aims to allow programs running on a small device such as a raspberry pi to be able to draw graphics using a simple API and without needing the X window system.

## Dependencies
This fork does not require any dependencies to run. Only `gcc`, `glibc`, and a GNU/Linux system are required.

## Building the demo
```bash
make
```

## Running
Make sure to run the program directly on a virtual tty and not a graphical session.
```bash
./fbdemo
```

## Licence
MIT
