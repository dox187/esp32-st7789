# ST7789 example

ST7789 display driver and demo code.

[![Video](http://img.youtube.com/vi/Wql4WTpPrWs/maxresdefault.jpg)](https://youtu.be/Wql4WTpPrWs)


### How to build:
```
[]$ idf.py set-target {yourESPchip}
[]$ idf.py menuconfig
[]$ idf.py flash monitor
```
Set the necessary values in menuconfig.  

### Changelog
 - The required global values are defined in Kconfig.projbuild.
 - Self config for the driver 
 - Default gamma *(later, this as a setting will also be moved to menuconfig)*
 - offset X and Y parameters. *(especially because of the "1.69″ ips 240X280 SPI panel")*
 - in case of rotation, it "rotates" the width and height values (and their corresponding offset values)
 - SPI DMA and SPI Frequency can be set in menuconfig

### What is left
 - adjustable backlight brightness in at least 256 steps
 - functions to use 12-bit (rgb444) color depth *(A full-frame can be updated much (33%) faster, is practical for grayscale content or content that uses just a few colors)*
 - interlaced rendering
 - Frame rate control: adjustable *refresh rate* in menuconfig *(the default is 60hz)*