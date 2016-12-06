# Symdep

This program recursively checks symbol dependencies of prebuilt proprietary ELF objects against compiled Android ROM

## Purpose

When you build Android ROM using vendor blobs due to lack of sourcecode, you could often get linker's "cannot locate symbol..." errors.
This tool lets you check your proprietary prebuilt ELF objects against your compiled Android ROM before flashing ROM and attempting to boot.
You will get list of all missing symbols for target ELF object one at a time, unlike linker's one after other attempts to load object.

## How it works

Assumption: you have successfully built Android ROM and you have a full set of files in 'out/target/product//system' directory,
including prebuilt vendor's objects.

Run Symdep against your target blob which should be located in one of these directories:
```
	out/target/product//system/bin
	out/target/product//system/lib
	out/target/product//system/lib64
	out/target/product//system/vendor/lib
	out/target/product//system/vendor/lib64
```
Symdep will read ELF structure, determine external symbols and attempt to locate them in needed objects from directories above.

## Usage
```
There are few options:
 -v, --verbose      Will show you all found symbols and objects in which they were found

 -s, --silent       Shows final result only

 --depth <n>        Sets recursion depth to <n>, default value is 1.
                    Could be useful when one prebuilt blob uses other prebuilt blob.
                    So you can check all chain at once.

 --full             Full depth recursion. Checks full chain of dependencies.
                    This can take a lot of time.

 -i <path>          Includes custom paths where to look for shared objects in addition to mentioned above.
                    Use colon-separated list in case of multiple values.
                    For instance: -i directory1:directory2:directory3

 --shim <lib|shim>  Supplies shim counterpart for shared object
                    Use colon-separated list in case of multiple values.
                    For instance: --shim "lib1.so|lib1_shim.so:lib2.so|lib2_shim.so"
                    Don't forget to enclose option's value in quotes,
                    otherwise shell will interpret it as piping and you'll get an error.

 --demangle         Decode low-level symbol names into user-level names

 -h, --help         Display help information
```
## How to make

This program requires binutils-dev package
```bash
	sudo apt-get install binutils-dev
```
Compile using gcc:
```bash
gcc symdep.c -lbfd -o symdep
```

## Examples
```bash
~ $ ./symdep cm13/out/target/product/hwp6s/system/lib/libcamera_core.so
cm13/out/target/product/hwp6s/system/lib/libcamera_core.so
    libbinder.so
    libc.so
    libcamera_client.so
    libcutils.so
    libdl.so
    libexif.so
    libft2.so
    libgui.so
    libhardware.so
    libjpeg.so
    libk3jpeg.so
    libm.so
    libskia.so
    libstdc++.so
    libutils.so

Cannot locate symbols:
libcamera_core.so -> _ZN7android13SensorManagerC1Ev
libcamera_core.so -> _ZN7android9SingletonINS_13SensorManagerEE5sLockE
libcamera_core.so -> _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE
libcamera_core.so -> exif_entry_gps_initialize
libcamera_core.so -> _ZN7android13SensorManager16createEventQueueEv
```
Let's add shim library for libexif.so:
```bash
~ $ ./symdep --shim "libexif.so|libexif_shim.so" cm13/out/target/product/hwp6s/system/lib/libcamera_core.so
cm13/out/target/product/hwp6s/system/lib/libcamera_core.so
    libbinder.so
    libc.so
    libcamera_client.so
    libcutils.so
    libdl.so
    libexif.so
    libexif_shim.so
    libft2.so
    libgui.so
    libhardware.so
    libjpeg.so
    libk3jpeg.so
    libm.so
    libskia.so
    libstdc++.so
    libutils.so

Cannot locate symbols:
libcamera_core.so -> _ZN7android13SensorManagerC1Ev
libcamera_core.so -> _ZN7android9SingletonINS_13SensorManagerEE5sLockE
libcamera_core.so -> _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE
libcamera_core.so -> _ZN7android13SensorManager16createEventQueueEv
```
At this time "exif_entry_gps_initialize" was found.

Let's demangle missing symbols:
```bash
~ $ ./symdep --demangle --shim "libexif.so|libexif_shim.so" cm13/out/target/product/hwp6s/system/lib/libcamera_core.so
cm13/out/target/product/hwp6s/system/lib/libcamera_core.so
    libbinder.so
    libc.so
    libcamera_client.so
    libcutils.so
    libdl.so
    libexif.so
    libexif_shim.so
    libft2.so
    libgui.so
    libhardware.so
    libjpeg.so
    libk3jpeg.so
    libm.so
    libskia.so
    libstdc++.so
    libutils.so

Cannot locate symbols:
libcamera_core.so -> _ZN7android13SensorManagerC1Ev
                     android::SensorManager::SensorManager()
libcamera_core.so -> _ZN7android9SingletonINS_13SensorManagerEE5sLockE
                     android::Singleton<android::SensorManager>::sLock
libcamera_core.so -> _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE
                     android::Singleton<android::SensorManager>::sInstance
libcamera_core.so -> _ZN7android13SensorManager16createEventQueueEv
                     android::SensorManager::createEventQueue()
```