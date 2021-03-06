/*  =========================================================================
    hydra_client - Hydra Client

    ** WARNING *************************************************************
    THIS SOURCE FILE IS 100% GENERATED. If you edit this file, you will lose
    your changes at the next build cycle. This is great for temporary printf
    statements. DO NOT MAKE ANY CHANGES YOU WISH TO KEEP. The correct places
    for commits are:

     * The XML model used for this code generation: hydra_client.xml, or
     * The code generation script that built this file: zproto_client_c
    ************************************************************************
    Copyright (c) the Contributors as noted in the AUTHORS file.       
    This file is part of zbroker, the ZeroMQ broker project.           
                                                                       
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.           
    =========================================================================
*/

#ifndef HYDRA_CLIENT_H_INCLUDED
#define HYDRA_CLIENT_H_INCLUDED

#include <czmq.h>

#ifdef __cplusplus
extern "C" {
#endif

//  Opaque class structure
#ifndef HYDRA_CLIENT_T_DEFINED
typedef struct _hydra_client_t hydra_client_t;
#define HYDRA_CLIENT_T_DEFINED
#endif

//  @interface
//  Create a new hydra_client, return the reference if successful, or NULL
//  if construction failed due to lack of available memory.
hydra_client_t *
    hydra_client_new (void);

//  Destroy the hydra_client and free all memory used by the object.
void
    hydra_client_destroy (hydra_client_t **self_p);

//  Return actor, when caller wants to work with multiple actors and/or
//  input sockets asynchronously.
zactor_t *
    hydra_client_actor (hydra_client_t *self);

//  Return message pipe for asynchronous message I/O. In the high-volume case,
//  we send methods and get replies to the actor, in a synchronous manner, and
//  we send/recv high volume message data to a second pipe, the msgpipe. In
//  the low-volume case we can do everything over the actor pipe, if traffic
//  is never ambiguous.
zsock_t *
    hydra_client_msgpipe (hydra_client_t *self);

//  Return true if client is currently connected, else false. Note that the
//  client will automatically re-connect if the server dies and restarts after
//  a successful first connection.
bool
    hydra_client_connected (hydra_client_t *self);

//  Connect to server endpoint, with specified timeout in msecs (zero means wait    
//  forever). Constructor succeeds if connection is successful. The sink endpoint is
//  provided by the node's own server, for storing received posts.                  
//  Returns >= 0 if successful, -1 if interrupted.
int 
    hydra_client_connect (hydra_client_t *self, const char *endpoint, uint32_t timeout);

//  Start synchronization with server. This method returns immediately, and then    
//  signals progress via the msgpipe socket, with POST, SUCCESS, and FAILED         
//  commands.                                                                       
//  Returns >= 0 if successful, -1 if interrupted.
int 
    hydra_client_sync (hydra_client_t *self);

//  Return last received status
int 
    hydra_client_status (hydra_client_t *self);

//  Return last received nickname
const char *
    hydra_client_nickname (hydra_client_t *self);

//  Return last received reason
const char *
    hydra_client_reason (hydra_client_t *self);

//  Self test of this class
void
    hydra_client_test (bool verbose);

//  To enable verbose tracing (animation) of hydra_client instances, set
//  this to true. This lets you trace from and including construction.
extern volatile int
    hydra_client_verbose;
//  @end

#ifdef __cplusplus
}
#endif

#endif
