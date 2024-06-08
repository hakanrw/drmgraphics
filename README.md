# DRM Graphics
This is a very simple C library for drawing simple shapes and text to screen using DRM/KMS.
The original project from 2017 used `/dev/fb0` as a backend, which required Linux. This project on the other hand,
uses the more portable DRM/KMS backend, supporting both Linux and BSD systems.


This project is a cheap amalgamation of [richinfante/framebuffergraphics](https://github.com/richinfante/framebuffergraphics) and
[girish2k44/drmmodeset](https://github.com/girish2k44/drmmodeset). The former provides drawing logic, and the latter provides DRM functionality.


## Quoted from richinfante (regarding the original project)

**This is a modified version of a really old project I build back in high school. I'm still in progress of making it more modern and fast, and at the moment, it compiles, but is most likely very buggy**

At it's heart, this project aims to allow programs running on a small device such as a raspberry pi to be able to draw graphics using a simple API and without needing the X window system.

## Dependencies
This project does not require any dependencies to run. You just need a C compiler.

## Building the demo
```sh
make
```

## Running
Make sure to run the program directly on a virtual tty and not a graphical session.
```sh
./fbdemo
```

## Licence
MIT

Copyright (c) 2024 Hakan Candar
Copyright (c) 2017 Rich Infante
Copyright (c) 2014 Girish Sharma
