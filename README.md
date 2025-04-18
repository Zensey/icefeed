* Proper Timebase Conversion:
    
    Converts input timestamps to output stream's timebase
    Maintains relative timing between packets
    Preserves audio synchronization

* Running Counter (next_pts):

    Global counter that never decreases
    Ensures no timestamp collisions between files
    Always increments by at least 1

* Audio-Specific Handling:

    Sets DTS equal to PTS (correct for audio streams)
    Handles cases where PTS might be missing


