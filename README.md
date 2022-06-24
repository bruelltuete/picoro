# Hacky coroutine library for the Raspi Pico

I needed a way to jump between different "threads" of execution, without the overhead of real multi-threading.
Simple cooperative multi-tasking was good enough.

## Buyer beware

* This started out as a C library (to mimic the pico-sdk) but eventually more and more C++ crept in. So it's now this messy mish-mash of C/C++. Eventually I'll get around to fix some of that.
* No attempt at API stability, feel free to fork and change in any way you see fit.
* This lib is for me hacking on a lazy weekend afternoon, along the same line as the point above.
* Feel free to suggest better ways of doing this.
