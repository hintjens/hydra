/*  =========================================================================
    hydra_server - Hydra Server (in C)

    Copyright (c) the Contributors as noted in the AUTHORS file.       
    This file is part of zbroker, the ZeroMQ broker project.           
                                                                       
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.           
    =========================================================================
*/

/*
@header
    This is a simple client API for the Hydra protocol.
@discuss
    Detailed discussion of the class, if any.
    todo
    - create PULL socket for receiving posts
    - bind to some inproc endpoint, passed to each client
    - all clients create PUSH socket and connect to this
    - clients send incoming posts to this socket
    - saved by server
    
@end
*/

#include "hydra_classes.h"

//  Maximum size of a content we'll work with for now is 10MB
//  We should use chunking / credit based flow control for larger files
#define CONTENT_MAX_SIZE    10 * 1024 * 1024

//  ---------------------------------------------------------------------------
//  Forward declarations for the two main classes we use here

typedef struct _server_t server_t;
typedef struct _client_t client_t;

//  This structure defines the context for each running server. Store
//  whatever properties and structures you need for the server.

struct _server_t {
    zsock_t *pipe;              //  Actor pipe back to caller
    zconfig_t *config;          //  Current loaded configuration
    hydra_ledger_t *ledger;     //  Posts ledger
    zsock_t *sink;              //  Sink socket
};

//  ---------------------------------------------------------------------------
//  This structure defines the state for each client connection. It will
//  be passed to each action in the 'self' argument.

struct _client_t {
    server_t *server;           //  Reference to parent server
    hydra_proto_t *message;     //  Message from and to client
    hydra_ledger_t *ledger;     //  Posts ledger, same as server ledger
    hydra_post_t *post;         //  Current post we're sending
    int oldest_index;           //  Oldest post held by client
    int newest_index;           //  Newest post held by client
};

//  Include the generated server engine
#include "hydra_server_engine.inc"

static int
    s_server_handle_sink (zloop_t *loop, zsock_t *reader, void *argument);
static zmsg_t *
    s_server_store_post (server_t *self, zmsg_t *msg);

//  Allocate properties and structures for a new server instance.
//  Return 0 if OK, or -1 if there was an error.

static int
server_initialize (server_t *self)
{
    //  If server does not have an identity yet, generate one
    char *identity = zconfig_resolve (self->config, "/hydra/identity", NULL);
    if (!identity) {
        zuuid_t *uuid = zuuid_new ();
        zconfig_put (self->config, "/hydra/identity", zuuid_str (uuid));
        zconfig_put (self->config, "/hydra/nickname", "Anonymous");
        zconfig_save (self->config, "hydra.cfg");
        zuuid_destroy (&uuid);
    }
    //  Load post ledger
    self->ledger = hydra_ledger_new ();
    hydra_ledger_load (self->ledger);

    //  Create and bind sink socket (posts will come here)
    self->sink = zsock_new (ZMQ_PULL);
    char endpoint [32];
    while (true) {
        sprintf (endpoint, "inproc://hydra-%04x-%04x",
                randof (0x10000), randof (0x10000));
        if (zsock_bind (self->sink, "%s", endpoint) == 0)
            break;
    }
    engine_handle_socket (self, self->sink, s_server_handle_sink);
    return 0;
}

//  Free properties and structures for a server instance

static void
server_terminate (server_t *self)
{
    hydra_ledger_destroy (&self->ledger);
    zsock_destroy (&self->sink);
}

//  Process server API method, return reply message if any
//  SINK - create and bind sink PULL socket, return inproc endpoint
//  POST - store post

static zmsg_t *
server_method (server_t *self, const char *method, zmsg_t *msg)
{
    zmsg_t *reply = NULL;
    if (streq (method, "SINK")) {
        reply = zmsg_new ();
        zmsg_addstr (reply, zsock_endpoint (self->sink));
    }
    else
    if (streq (method, "POST"))
        reply = s_server_store_post (self, msg);
    else {
        zsys_error ("unknown server method '%s' - failure", method);
        assert (false);
    }
    return reply;
}

static zmsg_t *
s_server_store_post (server_t *self, zmsg_t *msg)
{
    char *subject = zmsg_popstr (msg);
    hydra_post_t *post = hydra_post_new (subject);

    char *parent_id = zmsg_popstr (msg);
    hydra_post_set_parent_id (post, parent_id);
    zstr_free (&parent_id);

    char *mime_type = zmsg_popstr (msg);
    hydra_post_set_mime_type (post, mime_type);
    zstr_free (&mime_type);

    char *arg_type = zmsg_popstr (msg);
    if (streq (arg_type, "string")) {
        char *content = zmsg_popstr (msg);
        hydra_post_set_content (post, content);
        zstr_free (&content);
    }
    else
    if (streq (arg_type, "file")) {
        char *filename = zmsg_popstr (msg);
        hydra_post_set_file (post, filename);
        zstr_free (&filename);
    }
    else
    if (streq (arg_type, "frame")) {
        zframe_t *frame = zmsg_pop (msg);
        hydra_post_set_data (post, zframe_data (frame), zframe_size (frame));
        zframe_destroy (&frame);
    }
    else {
        zsys_error ("bad argument type=%s - failure", arg_type);
        assert (false);
    }
    zstr_free (&arg_type);
    zstr_free (&subject);

    zmsg_t *reply = zmsg_new ();
    zmsg_addstr (reply, hydra_post_ident (post));
    hydra_ledger_store (self->ledger, &post);
    return reply;
}

static int
s_server_handle_sink (zloop_t *loop, zsock_t *reader, void *argument)
{
    //  Message is a single post object passed by reference
    server_t *self = (server_t *) argument;
    hydra_post_t *post;
    zsock_recv (reader, "p", &post);
    hydra_ledger_store (self->ledger, &post);
    return 0;
}


//  Allocate properties and structures for a new client connection and
//  optionally engine_set_next_event (). Return 0 if OK, or -1 on error.

static int
client_initialize (client_t *self)
{
    //  Oldest to newest is the range of posts the client has told us it has.
    //  The lowest valid index is 0, When the client has no posts, these are
    //  set to -1. Index points into server ledger.
    self->oldest_index = self->newest_index = -1;
    self->ledger = self->server->ledger;
    return 0;
}

//  Free properties and structures for a client connection

static void
client_terminate (client_t *self)
{
    hydra_post_destroy (&self->post);
}


//  ---------------------------------------------------------------------------
//  set_server_identity
//

static void
set_server_identity (client_t *self)
{
    char *identity = zconfig_resolve (self->server->config, "/hydra/identity", NULL);
    char *nickname = zconfig_resolve (self->server->config, "/hydra/nickname", "");
    assert (identity);
    hydra_proto_set_identity (self->message, identity);
    hydra_proto_set_nickname (self->message, nickname);
}


//  ---------------------------------------------------------------------------
//  calculate_status_for_client
//

static void
calculate_status_for_client (client_t *self)
{
    //  Map oldest/newest post IDs into ledger indices
    self->oldest_index = hydra_ledger_index (self->ledger,
                                             hydra_proto_oldest (self->message));
    self->newest_index = hydra_ledger_index (self->ledger,
                                             hydra_proto_newest (self->message));

    if (self->oldest_index == -1 || self->newest_index == -1) {
        //  Either we have a valid range or we don't
        self->oldest_index = self->newest_index = -1;
        //  If upper is -1, number of posts after index is size
        hydra_proto_set_after (self->message, hydra_ledger_size (self->ledger));
    }
    else {
        //  Number of posts before oldest index is same as index
        hydra_proto_set_before (self->message, self->oldest_index);
        //  Number of posts after upper index is size - upper - 1
        hydra_proto_set_after (self->message,
            hydra_ledger_size (self->ledger) - self->newest_index - 1);
    }
}


//  ---------------------------------------------------------------------------
//  fetch_specified_post_header
//

static void
fetch_specified_post_header (client_t *self)
{
    hydra_post_destroy (&self->post);
    int last_index = hydra_ledger_size (self->ledger) - 1;
    
    switch (hydra_proto_which (self->message)) {
        case HYDRA_PROTO_FETCH_OLDER:
            //zsys_info ("fetch older, oldest_index=%d", self->oldest_index);
            if (self->oldest_index >= 0)
                self->post = hydra_ledger_fetch (self->ledger, --self->oldest_index);
        break;
        
        case HYDRA_PROTO_FETCH_NEWER:
            //zsys_info ("fetch newer, newest_index=%d last_index=%d", self->newest_index, last_index);
            if (self->newest_index < last_index)
                self->post = hydra_ledger_fetch (self->ledger, ++self->newest_index);
        break;
        
        case HYDRA_PROTO_FETCH_RESET:
            //zsys_info ("fetch reset, last_index=%d", last_index);
            self->newest_index = last_index;
            self->oldest_index = last_index;
            self->post = hydra_ledger_fetch (self->ledger, self->newest_index);
        break;
    }
    if (self->post)
        hydra_post_encode (self->post, self->message);
    else
        engine_set_exception (self, no_such_post_event);
}


//  ---------------------------------------------------------------------------
//  fetch_specified_post_chunk
//

static void
fetch_specified_post_chunk (client_t *self)
{
    zchunk_t *chunk = hydra_post_fetch (self->post,
        hydra_proto_octets (self->message), hydra_proto_offset (self->message));
    hydra_proto_set_content (self->message, &chunk);
}


//  ---------------------------------------------------------------------------
//  signal_command_invalid
//

static void
signal_command_invalid (client_t *self)
{
    hydra_proto_set_status (self->message, HYDRA_PROTO_COMMAND_INVALID);
}


//  ---------------------------------------------------------------------------
//  Selftest

void
hydra_server_test (bool verbose)
{
    printf (" * hydra_server: ");
    if (verbose)
        printf ("\n");
    
    //  @selftest
    zactor_t *server = zactor_new (hydra_server, "server");
    if (verbose)
        zstr_send (server, "VERBOSE");
    zstr_sendx (server, "LOAD", "hydra.cfg", NULL);
    zstr_sendx (server, "BIND", "ipc://@/hydra_server", NULL);

    zsock_t *client = zsock_new (ZMQ_DEALER);
    assert (client);
    zsock_set_rcvtimeo (client, 2000);
    zsock_connect (client, "ipc://@/hydra_server");

    hydra_proto_t *message = hydra_proto_new ();
    hydra_proto_set_id (message, HYDRA_PROTO_HELLO);
    hydra_proto_send (message, client);
    hydra_proto_recv (message, client);
    assert (hydra_proto_id (message) == HYDRA_PROTO_HELLO_OK);
    hydra_proto_destroy (&message);
    
    zsock_destroy (&client);
    zactor_destroy (&server);
    //  @end
    printf ("OK\n");
}
