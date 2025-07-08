# TWANG32
An ESP32 based, 1D, LED strip, dungeon crawler. inspired by Line Wobbler by Robin B

This was ported from the [TWANG fork](https://github.com/bdring/TWANG) by bdring of [Buildlog.net Blog](http://www.buildlog.net/blog?s=twang).  
Modifications and bug fixes by [Janek](https://janek.ing).

[![Youtube Video](http://www.buildlog.net/blog/wp-content/uploads/2018/05/vid_thumb.png)](https://www.youtube.com/watch?v=RXpfa-ZvUMA)

![TWANG LED Game](http://www.buildlog.net/blog/wp-content/uploads/2018/01/20180111_130909-1.jpg?s=200)

## Why ESP32?
- Lower Cost than Arduino Mega
- Faster Processor
- More Memory
- Smaller (allows a smaller enclosure that is easier to print and more portable)
- DAC pins for better sound capabilities.
- WiFi and Bluetooth.

**Current State**
- All of the Arduino version game features are functional.
- The game now has a WiFi access port to get game stats. Connect a smartphone or computer to see them.
    - **SSID:** TWANG_AP
    - **Password:** 12345666
    - **URL:** 192.168.4.1
- You can update these settings over WiFi
    - LED Count
    - LED Brightness
    - Audio Volume
    - Joystick Deadzone (removes drift)
    - Attack Threshold (twang sensitivity)
    - Lives Per Level

![](http://www.buildlog.net/blog/wp-content/uploads/2018/03/20180328_122254.jpg)

## Changes by Janek
- PlatformIO project setup (though it probably still compiles in Arduino IDE as well)
- optional two MPU6050 setup, for a better handheld experience (the enclose does not need to rest on a table anymore!)
- made attack more consistant (by always attacking all lit up spaces fully)
- new levels
- a lot of level tweaks to make them fairer and more interesting
- better conveyor rendering
- rendering of spawners
- fix life display
- fix configuration over serial
- more screensavers
- remove running median dependency
- add option to set led offset and screensaver brightness
- many small code quality improvements

## Required libraries:
* [FastLED](http://fastled.io/)

## Hardware used:
* ESP32, I typically use the NodeMCU-32S module
* LED light strip. The more the better, maximum of 1000. Tested with 1x & 2x 144/meter, 12x 60/meter and 5m x 114/meter strips. This has been tested with APA102C and NeoPixel (WS2812) type strips. Anything compatible with the FastLED library should work.
    * For WS2812 strips, I usually recommend the ones by BTF lighting, e.g. [these](https://www.amazon.de/dp/B088FK8NG6)
* [MPU6050 accelerometer](https://www.amazon.de/dp/B07N2ZL34Z)
* Spring doorstop, bdring used [these](http://smile.amazon.com/gp/product/B00J4Y5BU2), in Germany you can get [these](https://www.amazon.de/dp/B07PFFFFMT)
* Speaker and amplifier. I use a [PAM8403 module](https://www.amazon.de/dp/B07KQCKWF8). (ESP32 cannot drive a speaker as loudly as an Arduino)
* The cheapest place to get the electronic components probably is AliExpress

See [Buildlog.net Blog](http://www.buildlog.net/blog?s=twang) for more details.

## Enclosure
You have two options:
1. The original one by bdring, which you can find as an [STL here](http://www.buildlog.net/blog/wp-content/uploads/2018/04/twang32_stl.zip) or as [STEP in this repo](/parts/step/original/)
1. [The one by Janek](/parts/step/usb-speakerless/), which has several modifications:
    1. No speaker and therefore slightly smaller (and slightly more water resistant)
    1. Made to connect using [these USB-C sockets](https://www.amazon.de/dp/B0CPLRH4W6)
    1. Inside mounting holes perfect for a 20x20 2,54mm stripboard
    1. Screw mounted MPU 6050 (instead of using a foam block to hold it down)
    1. Screw holes for fixing the knob and spring (since they like to twist when playing, which can skew the gyro readings)
    1. All holes ready for [M3 self-tapering screws](https://www.amazon.de/dp/B0DGGC9ZYC) (though regular M3 screws will work as well)
    1. A small external splitter/junction box, so you can add the power supply separately and closer to the LED strip

![TWANG32](http://www.buildlog.net/blog/wp-content/uploads/2018/03/twang32_enclosure.jpg)

## PINs
- 16 - LED Data
- 17 - LED Clock (optional, for the strips that need it)
- 21 - Accelerometer SDA (or the equivalent I²C pin on your board)
- 22 - Accelerometer SCL (or the equivalent I²C pin on your board)
- 25 - DAC (analog audio out, unamplified)

## TO DO List:
- Wireless features
    - 2 Player features by linking controllers. TBD
-  Settings:
    - Change strip type.
-  Digitized Audio
    - Currently the port uses the same square wave tones of the the Arduino version.
    - I want to convert to digitized high quality sound effects.
    - Possibly mix multiple sounds so things like lava and movement sound good at the same time.
- Better looking mobile web interface (looks more like a web app)

## Overview
The following is a quick overview of the code to help you understand and tweak the game to your needs.

The game is played on a 1000 unit line, the position of enemies, the player, lava etc range from 0 to 1000 and the LEDs that represent them are derived using the `getLED()` function. You don't need to worry about this but it's good to know for things like the width of the attack and player max move speed. Regardless of the number of LEDs, everything takes place in this 1000 unit wide line.

**LED SETUP** Defines the quantity of LEDs as well as the data and clock pins used. I've tested several APA102-C strips and the color order sometimes changes from BGR to GBR, if the player is not blue, the exit green and the enemies red, this is the bit you want to change. Brightness should range from 50 to 255, use a lower number if playing at night or wanting to use a smaller power supply. `DIRECTION` can be set to 0 or 1 to flip the game orientation. In `setup()` there is a `FastLED.addLeds()` line, in there you could change it to another brand of LED strip like the cheaper WS2812.

The game also has 3 regular LEDs for life indicators (the player gets 3 lives which reset each time they level up). The pins for these LEDs are stored in `lifeLEDs[]` and are updated in the `updateLives()` function.

**JOYSTICK SETUP** All parameters are commented in the code, you can set it to work in both forward/backward as well as side-to-side mode by changing `JOYSTICK_ORIENTATION`. Adjust the `ATTACK_THRESHOLD` if the "Twanging" is overly sensitive and the `JOYSTICK_DEADZONE` if the player slowly drifts when there is no input (because it's hard to get the MPU6050 dead level).

**WOBBLE ATTACK** Sets the width, duration (ms) of the attack.

**POOLS** These are the object pools for enemies, particles, lava, conveyors etc. You can modify the quantity of any of them if your levels use more or if you want to save some memory, just remember to update the respective counts to avoid errors.

**USE_GRAVITY** 0/1 to set if particles created by the player getting killed should fall towards the start point, the `BEND_POINT` variable can be set to mark the point at which the strip of LEDs goes from being horizontal to vertical. The game is 1000 units wide (regardless of number of LED's) so 500 would be the mid point. If this is confusing just set `USE_GRAVITY` to 0.

## Modifying / Creating levels
Find the `loadLevel()` function, in there you can see a switch statement with the existing levels and a comment with more description for creating levels.