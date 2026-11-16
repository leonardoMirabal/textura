More free memory for what comes next (28 KB free instead of 3 KB), with the same sound and the same screens.

- The screen is drawn in two strips instead of one large buffer: every page looks the same, 15 KB freed.
- Some of the app's data moved into unused fast RAM: 9.7 KB freed.
- New `make fm1-dbg`: reads the FM-1's debug figures (stack use, screen draw time) over USB.
