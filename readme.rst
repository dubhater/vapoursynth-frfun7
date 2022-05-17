Description
===========

Frfun7 is a spatial fractal denoising plugin by Copyright (C) 2002-2006, 2013 Marc Fauconneau (prunedtree), (C)2021 Ferenc Pintér.


Usage
=====
::

    frfun7.Frfun7(clip clip[, float l=1.1, float t=6.0, float tuv=2.0, int p=0, int tp1=0, int r1=3, int opt=1])


Parameters:
    *clip*
        A clip to process. It must be 8 bit Gray or YUV.

    *l*
        It should be called "lambda" but that word is reserved by Python.

        Adjusts the power of the local denoising.
        
        It must not be negative.

        Default: 1.1.

    *t*
        Limits the maximum luma denoising power for edges.

        0 disables processing of the luma plane.

        It must not be negative.

        Default: 6.0.

    *tuv*
        Limits the maximum chroma denoising power for edges.

        0 disables processing of the chroma planes.

        It must not be negative.

        Default: 2.0.

    *p*
        Selects the type of filtering.

        0 - the basic algorithm

        1 - adaptive overlapping

        2 - temporal

        4 - adaptive radius

        Default: 0.

    *tp1*
        A threshold which affects p=1. Values greater than 0 will make it skip processing some pixels.

        Default: 0.

    *r1*
        Radius for first pass of the internal algorithm.

        It can be 2 or 3. 2 is faster.

        Default: 3.


Compilation
===========

::

    meson build
    ninja -C build


License
=======

GPLv2.
