# ffmpeg-vulkan
An example of using the FFMPEG library for vulkan h264 decode to YUV420p.

Also supports other hardware if available

Adding scaling if specified

I have no knowledge of FFMPEG so take this example with a pinch of salt

## Docker

You can use the supplied Dockerfile to create an image that will build and run the example

`docker build -t vulkan-builder .`

Run the container with:
`docker run -it -v "$(pwd)":/src --device /dev/dri vulkan-builder`

Build in the container:
`cd /src; mkdir build; cd build; cmake ..; make -j; cd -; ./build/example vulkan`

