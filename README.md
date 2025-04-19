## icefeed: a simple source for icestream2

### ABOUT  

This is a a simple source for icestream2.
It created a AAC stream from local file system and feeds it to icecast2 server.
The playlist is randomized on each play cycle.
No transcoding is done, that's why M4A / MP4 files are only used.
Note that the target Icecast stream should be AAC.

### Install

    `make icefeed`

## Run the daemon

    `./icefeed icecast://source:password@icecast2.server:8000/stream.aac ~/music`
