## ACPIBacklight by RehabMan


### How to Install:

Install the kext using your favorite kext installer utility, such as Kext Wizard.  The Debug directory is for troubleshooting only, in normal "working" installs, you should install the Release version.


### Downloads:

Downloads are available on bitbucket:

https://bitbucket.org/RehabMan/os-x-acpi-backlight/downloads


### Build Environment

My build environment is currently Xcode 5.0.2, using SDK 10.8, targeting OS X 10.6.

No other build environment is supported.


### 32-bit Builds

Currently, builds are provided only for 64-bit systems.  32-bit/64-bit FAT binaries are not provided.  But you may be able build your own should you need them.  I do not test 32-bit, and there may be times when the repo is broken with respect to 32-bit builds.

Here's how to build 32-bit (universal):

- xcode 4.6.3
- open ACPIBacklight.xcodeproj
- click on ACPIBacklight at the top of the project tree
- select ACPIBacklight under Project
- change Architectures to 'Standard (32/64-bit Intel)'

probably not necessary, but a good idea to check that the targets don't have overrides:
- multi-select all the Targets
- check/change Architectures to 'Standard (32/64-bit Intel)'
- build (either w/ menu or with make)

Or, if you have the command line tools installed, just run:

- For FAT binary (32-bit and 64-bit in one binary)
make BITS=3264

- For 32-bit only
make BITS=32


### Source Code:

The source code is maintained at the following sites:

https://code.google.com/p/os-x-acpi-backlight/

https://github.com/RehabMan/OS-X-ACPI-Backlight

https://bitbucket.org/RehabMan/os-x-acpi-backlight



### Feedback:

Please use this thread on tonymacx86.com for feedback, questions, and help:

http://www.tonymacx86.com/hp-probook-mavericks/118805-full-range-brightness-using-acpibacklight.html



### Known issues:

- None yet.


### Change Log:

2013-12-21 v2.0

- Modified by RehabMan for use with the HP ProBook and custom DSDT patches.

- Save/Restore current brightness level in "NVRAM" for restoration across restarts.

- Various bugs/memory leaks/etc fixed.

- Implement interop for 'ioio' so that RawBacklight and various other params can be tweaked on the fly via bash.

- Implement support for XBQC/XBCM which allows setting of values "in between" those returned by _BCL.

- Implement smooth transitions between levels, just like a real MacBook[Air/Pro]

- Bump version to 2.0 given the significant changes



### History

See original post at:
http://www.insanelymac.com/forum/topic/268219-acpi-backlight-driver/

This version is enhanced by RehabMan for various features called out in the Change log, above.

Originally, we had little reason to use this kext on the HP ProBook, becuase the ACPI methods are broken on the ProBook series (they attempt to call back into Windows).  the combination of native brightness with PNLF patch and 'blinkscreen' was imperfect, but good enough.  But eventually, a method was discovered where by the hardware registers that control brightness on the HD3000/HD4000 can be manipulated directly from DSDT code.  For more information on these patches, see: https://github.com/RehabMan/HP-ProBook-4x30s-DSDT-Patch

Some of the details are covered in this thread: http://www.tonymacx86.com/hp-probook-mavericks/118805-full-range-brightness-using-acpibacklight.html


### Original Credits

The original driver was written by 'hotKoffy' and posted as an attachment to insanelymac.  There was a couple of different versions, including one posted by 'fxtentacle' where the values used by the driver were correctly scaled from the 0-0x400 range used by OS X.  I have incorporated those changes into the git history.

hotKoffy - original version
fxtentacle - 0-0x400 scaling fix
RehabMan - further enhancements
