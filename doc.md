                                #############################
                                ### STUFF WE NEED TO KNOW ###
                                #############################

reliable_state:

1. already_written:

    How many bytes of a slice are already written into the console.
    Slice-Segments might be written only partly in the function
    rel_output() by lib-call conn_ouput()

2. FLAGS

    * LAST_ALLOCATED_ALREADY_SENT
        Needed so that we know, when to create a new packet instead of fill up an old packet

    * SMALL_PACKET_ONLINE 
        Tells us if we have an unacknwolged partly full packet in our send-buffer.

3. recv_seqno && send_seqno
    Gives us the lower bound of our window. Up until this Sequence Number everything has been sent/recieved.


slice:

1. allocated:

    Says if the slice is currently used. If it's not allocated the slice segment can be overwritten.
