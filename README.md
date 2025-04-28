## icefeed: a client for broadcasting M4A format to an Icestream 2 server

### ABOUT

This is a client for broadcasting M4A format to an Icestream 2 server.
The main goal is to create an AAC stream from a local file system and feed it to Icecast 2 server.
The playlist is randomized on each play cycle.
No transcoding is done. Only M4A / MP4 files are used.
Note that the target Icecast 2 stream should be AAC.

### Dependencies

    apt install libavdevice-dev libavfilter-dev libswscale-dev libavcodec-dev libavformat-dev libswresample-dev libavutil-dev
    
### Build

    make icefeed

## Run the daemon

    ./icefeed icecast://source:password@icecast2.server:8000/stream.aac /home/user/music
