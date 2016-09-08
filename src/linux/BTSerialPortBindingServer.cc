/*
 * Copyright (c) 2012-2013, Eelco Cramer
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <v8.h>
#include <node.h>
#include <nan.h>
#include <node_buffer.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory>
#include <iostream>
#include "BTSerialPortBindingServer.h"

extern "C"{
    #include <stdio.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <stdlib.h>
    #include <signal.h>
    #include <termios.h>
    #include <sys/poll.h>
    #include <sys/ioctl.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <assert.h>


    #include <bluetooth/bluetooth.h>
    #include <bluetooth/hci.h>
    #include <bluetooth/hci_lib.h>
    #include <bluetooth/sdp.h>
    #include <bluetooth/sdp_lib.h>
    #include <bluetooth/rfcomm.h>
}

using namespace std;
using namespace node;
using namespace v8;

static uv_mutex_t write_queue_mutex;
static ngx_queue_t write_queue;

// BDADDR_ANY is defined as (&(bdaddr_t) {{0, 0, 0, 0, 0, 0}}) and 
// BDADDR_LOCAL is defined as (&(bdaddr_t) {{0, 0, 0, 0xff, 0xff, 0xff}}) which
// is the address of temporary thus not allowed in C++
static const bdaddr_t _BDADDR_ANY = {0, 0, 0, 0, 0, 0};
static const bdaddr_t _BDADDR_LOCAL = {0, 0, 0, 0xff, 0xff, 0xff};

// listen_baton_t resources are going to be released when closing the connection.
BTSerialPortBindingServer::listen_baton_t* BTSerialPortBindingServer::mListenBaton = nullptr;
// SDP connection is going to be closed once a client is connected
sdp_session_t * BTSerialPortBindingServer::mSdpSession = nullptr;
 
static int str2uuid( const char *uuid_str, uuid_t *uuid ) 
{
    uint32_t uuid_int[4];
    char *endptr;

    if( strlen( uuid_str ) == 36 ) {
        // Parse uuid128 standard format: 12345678-9012-3456-7890-123456789012
        char buf[9] = { 0 };

        if( uuid_str[8] != '-' && uuid_str[13] != '-' &&
            uuid_str[18] != '-'  && uuid_str[23] != '-' ) {
            return 0;
        }
        // first 8-bytes
        strncpy(buf, uuid_str, 8);
        uuid_int[0] = htonl( strtoul( buf, &endptr, 16 ) );
        if( endptr != buf + 8 ) 
            return 0;

        // second 8-bytes
        strncpy(buf, uuid_str+9, 4);
        strncpy(buf+4, uuid_str+14, 4);
        uuid_int[1] = htonl( strtoul( buf, &endptr, 16 ) );
        if( endptr != buf + 8 ) 
            return 0;

        // third 8-bytes
        strncpy(buf, uuid_str+19, 4);
        strncpy(buf+4, uuid_str+24, 4);
        uuid_int[2] = htonl( strtoul( buf, &endptr, 16 ) );
        if( endptr != buf + 8 ) 
            return 0;

        // fourth 8-bytes
        strncpy(buf, uuid_str+28, 8);
        uuid_int[3] = htonl( strtoul( buf, &endptr, 16 ) );
        if( endptr != buf + 8 ) 
            return 0;

        if( uuid != NULL ) 
            sdp_uuid128_create( uuid, uuid_int );
    } else if ( strlen( uuid_str ) == 8 ) {
        // 32-bit reserved UUID
        uint32_t i = strtoul( uuid_str, &endptr, 16 );
        if( endptr != uuid_str + 8 ) 
            return 0;
        if( uuid != NULL ) 
            sdp_uuid32_create( uuid, i );
    } else if( strlen( uuid_str ) == 4 ) {
        // 16-bit reserved UUID
        int i = strtol( uuid_str, &endptr, 16 );
        if( endptr != uuid_str + 4 ) 
            return 0;
        if( uuid != NULL ) 
            sdp_uuid16_create( uuid, i );
    } else {
        return 0;
    }

    return 1;
}


void BTSerialPortBindingServer::EIO_Listen(uv_work_t *req) {
    listen_baton_t * baton = static_cast<listen_baton_t *>(req->data);

    struct sockaddr_rc addr = {
        0x00,
        { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
        0x00
    };

    // allocate a socket
    baton->rfcomm->s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);

    // set the connection parameters (who to connect to)
    addr.rc_family = AF_BLUETOOTH;
    bacpy(&addr.rc_bdaddr, &_BDADDR_ANY);
    addr.rc_channel = (uint8_t) baton->listeningChannelID;

    baton->status = bind(baton->rfcomm->s, (struct sockaddr *)&addr, sizeof(addr));
    if(baton->status){
         sprintf(baton->errorString, "Couldn't bind bluetooth socket. errno:%d", errno);
         return;
    }

    baton->status = listen(baton->rfcomm->s, 1); // Bluetooth only accepts one connection at a time
    if(baton->status){
        sprintf(baton->errorString, "Couldn't listen on bluetooth socket. errno:%d", errno);
        return;
    }

    // Now, let's registar the service via SDP daemon
    Advertise(baton);

}

void BTSerialPortBindingServer::EIO_AfterListen(uv_work_t *req) {
    Nan::HandleScope scope;

    //std::unique_ptr<listen_baton_t> baton(static_cast<listen_baton_t *>(req->data));
    listen_baton_t * baton = static_cast<listen_baton_t *>(req->data);

    Nan::TryCatch try_catch;

    if (baton->status != 0) {
        Local<Value> argv[] = {
            Nan::Error(baton->errorString)
        };

        baton->ecb->Call(1, argv);
    }

    if (try_catch.HasCaught()) {
        Nan::FatalException(try_catch);
    }

    AsyncQueueWorker(new ClientWorker(baton->cb, baton));
}


void BTSerialPortBindingServer::EIO_Write(uv_work_t *req) {
    queued_write_t *queuedWrite = static_cast<queued_write_t*>(req->data);
    write_baton_t *data = static_cast<write_baton_t*>(queuedWrite->baton);

    BTSerialPortBindingServer* rfcomm = data->rfcomm;

    if (!rfcomm->mClientSocket || rfcomm->mClientSocket == -1) {
        sprintf(data->errorString, "Attempting to write to a closed connection");
    }

    data->result = write(rfcomm->mClientSocket, data->bufferData, data->bufferLength);    

    if (data->result != data->bufferLength) {
        sprintf(data->errorString, "Writing attempt was unsuccessful");
    }
}

void BTSerialPortBindingServer::EIO_AfterWrite(uv_work_t *req) {
    Nan::HandleScope scope;

    queued_write_t *queuedWrite = static_cast<queued_write_t*>(req->data);
    write_baton_t *data = static_cast<write_baton_t*>(queuedWrite->baton);

    Local<Value> argv[2];
    if (data->errorString[0]) {
        argv[0] = Nan::Error(data->errorString);
        argv[1] = Nan::Undefined();
    } else {
        argv[0] = Nan::Undefined();
        argv[1] = Nan::New<v8::Integer>((int32_t)data->result);
    }

    data->callback->Call(2, argv);

    uv_mutex_lock(&write_queue_mutex);
    ngx_queue_remove(&queuedWrite->queue);

    if (!ngx_queue_empty(&write_queue)) {
        // Always pull the next work item from the head of the queue
        ngx_queue_t* head = ngx_queue_head(&write_queue);
        queued_write_t* nextQueuedWrite = ngx_queue_data(head, queued_write_t, queue);
        uv_queue_work(uv_default_loop(), &nextQueuedWrite->req, EIO_Write, (uv_after_work_cb)EIO_AfterWrite);
    }
    uv_mutex_unlock(&write_queue_mutex);

    data->buffer.Reset();
    delete data->callback;
    data->rfcomm->Unref();
    delete data;
    delete queuedWrite;
}

void BTSerialPortBindingServer::EIO_Read(uv_work_t *req) {
    unsigned char buf[1024]= { 0 };

    read_baton_t *baton = static_cast<read_baton_t *>(req->data);

    memset(buf, 0, sizeof(buf));

    fd_set set;
    FD_ZERO(&set);
    FD_SET(baton->rfcomm->mClientSocket, &set);
    FD_SET(baton->rfcomm->rep[0], &set);

    int nfds = (baton->rfcomm->mClientSocket > baton->rfcomm->rep[0]) ? baton->rfcomm->mClientSocket : baton->rfcomm->rep[0];

    if (pselect(nfds + 1, &set, NULL, NULL, NULL, NULL) >= 0) {
        if (FD_ISSET(baton->rfcomm->mClientSocket, &set)) {
            baton->size = read(baton->rfcomm->mClientSocket, buf, sizeof(buf));
        } else {
            // when no data is read from rfcomm the connection has been closed.
            baton->size = 0;
        }

        // determine if we read anything that we can copy.
        if (baton->size > 0) {
            memcpy(baton->result, buf, baton->size);
        }
    }
}

void BTSerialPortBindingServer::EIO_AfterRead(uv_work_t *req) {
    Nan::HandleScope scope;

    read_baton_t *baton = static_cast<read_baton_t *>(req->data);

    Nan::TryCatch try_catch;

    Local<Value> argv[2];

    if (baton->size < 0) {
        argv[0] = Nan::Error("Error reading from connection");
        argv[1] = Nan::Undefined();
    } else {
        Local<Object> globalObj = Nan::GetCurrentContext()->Global();
        Local<Function> bufferConstructor = Local<Function>::Cast(globalObj->Get(Nan::New("Buffer").ToLocalChecked()));
        Handle<Value> constructorArgs[1] = { Nan::New<v8::Integer>(baton->size) };
        Local<Object> resultBuffer = bufferConstructor->NewInstance(1, constructorArgs);
        memcpy(Buffer::Data(resultBuffer), baton->result, baton->size);

        argv[0] = Nan::Undefined();
        argv[1] = resultBuffer;
    }

    baton->cb->Call(2, argv);

    if (try_catch.HasCaught()) {
        Nan::FatalException(try_catch);
    }

    baton->rfcomm->Unref();
    delete baton->cb;
    delete baton;

    baton = NULL;
}

void BTSerialPortBindingServer::Init(Handle<Object> target) {
    Nan::HandleScope scope;

    Local<FunctionTemplate> t = Nan::New<FunctionTemplate>(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(Nan::New("BTSerialPortBindingServer").ToLocalChecked());

    Nan::SetPrototypeMethod(t, "write", Write);
    Nan::SetPrototypeMethod(t, "read", Read);
    Nan::SetPrototypeMethod(t, "close", Close);

    target->Set(Nan::New("BTSerialPortBindingServer").ToLocalChecked(), t->GetFunction());
    target->Set(Nan::New("BTSerialPortBindingServer").ToLocalChecked(), t->GetFunction());
    target->Set(Nan::New("BTSerialPortBindingServer").ToLocalChecked(), t->GetFunction());
}

BTSerialPortBindingServer::BTSerialPortBindingServer() :
    s(0) {
}

BTSerialPortBindingServer::~BTSerialPortBindingServer() {

}

NAN_METHOD(BTSerialPortBindingServer::New) {
    uv_mutex_init(&write_queue_mutex);
    ngx_queue_init(&write_queue);

    if(info.Length() != 4){
        Nan::ThrowError("usage: BTSerialPortBindingServer(uuid: value, channel: value, successCallback, errorCallback)");
    }

    // String
    if(!info[0]->IsString()) {
        Nan::ThrowTypeError("First argument must be a string representing a UUID");
    }

    // callback
    if(!info[2]->IsFunction()) {
        Nan::ThrowTypeError("Second argument must be a function");
    }


    // callback
    if(!info[3]->IsFunction()) {
        Nan::ThrowTypeError("Third argument must be a function");
    }


    BTSerialPortBindingServer* rfcomm = new BTSerialPortBindingServer();
    rfcomm->Wrap(info.This());


    listen_baton_t * baton = new listen_baton_t();
    // I will release the memory in Close()
    mListenBaton = baton;
    String::Utf8Value uuid(info[0]);
    if(!str2uuid(*uuid, &baton->uuid)){
        Nan::ThrowError("The UUID is invalid");
    }

    baton->rfcomm = Nan::ObjectWrap::Unwrap<BTSerialPortBindingServer>(info.This());

    // allocate an error pipe
    if (pipe(baton->rfcomm->rep) == -1) {
        Nan::ThrowError("Cannot create pipe for reading.");
    }

    int flags = fcntl(baton->rfcomm->rep[0], F_GETFL, 0);
    fcntl(baton->rfcomm->rep[0], F_SETFL, flags | O_NONBLOCK);

    baton->cb = new Nan::Callback(info[2].As<Function>());
    baton->ecb = new Nan::Callback(info[3].As<Function>());
    baton->listeningChannelID = info[1]->Int32Value();
    baton->request.data = baton;
    baton->rfcomm->Ref();

    uv_queue_work(uv_default_loop(), &baton->request, EIO_Listen, (uv_after_work_cb)EIO_AfterListen);

    info.GetReturnValue().Set(info.This());
}


void BTSerialPortBindingServer::Advertise(listen_baton_t * baton) {

    uint8_t rfcomm_channel = (uint8_t) baton->listeningChannelID;

    const char *service_name = "RFCOMM Server socket";
    const char *service_dsc = "A RFCOMM listening socket";
    const char *service_prov = "Service Provider";

    uuid_t root_uuid, l2cap_uuid, rfcomm_uuid;
    sdp_list_t *l2cap_list = 0, 
               *rfcomm_list = 0,
               *root_list = 0,
               *proto_list = 0, 
               *access_proto_list = 0;
    sdp_data_t *channel = 0;

    sdp_record_t *record = sdp_record_alloc();

    sdp_set_service_id(record, baton->uuid);

    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    root_list = sdp_list_append(0, &root_uuid);
    sdp_set_browse_groups( record, root_list );

    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    l2cap_list = sdp_list_append( 0, &l2cap_uuid );
    proto_list = sdp_list_append( 0, l2cap_list );

    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
    channel = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
    rfcomm_list = sdp_list_append( 0, &rfcomm_uuid );
    sdp_list_append( rfcomm_list, channel );
    sdp_list_append( proto_list, rfcomm_list );

    // Attach protocol information to service record
    access_proto_list = sdp_list_append( 0, proto_list );
    sdp_set_access_protos( record, access_proto_list );

    // Set the name, provider, and description
    sdp_set_info_attr(record, service_name, service_prov, service_dsc);

    // Connect to the local SDP server
    mSdpSession = sdp_connect(&_BDADDR_ANY, &_BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
    if(mSdpSession == NULL){
        baton->status = -1;
        sprintf(baton->errorString, "Cannot connect to SDP Daemon. errno: %d", errno);
        return;
    }

    // Register the service record.
    if(sdp_record_register(mSdpSession, record, 0) == -1){
        baton->status = -1;
        sprintf(baton->errorString, "Cannot register SDP record. errno: %d", errno);
        return;
    }

    // cleanup
    sdp_data_free( channel );
    sdp_list_free( l2cap_list, 0 );
    sdp_list_free( rfcomm_list, 0 );
    sdp_list_free( root_list, 0 );
    sdp_list_free( access_proto_list, 0 );

}

NAN_METHOD(BTSerialPortBindingServer::Write) {
    // usage
    if (info.Length() != 2) {
        Nan::ThrowError("usage: write(buf, callback)");
    }

    // buffer
    if(!info[0]->IsObject() || !Buffer::HasInstance(info[0])) {
        Nan::ThrowTypeError("First argument must be a buffer");
    }

    Local<Object> bufferObject = info[0].As<Object>();
    char* bufferData = Buffer::Data(bufferObject);
    size_t bufferLength = Buffer::Length(bufferObject);

    // callback
    if(!info[1]->IsFunction()) {
        Nan::ThrowTypeError("Second argument must be a function");
    }

    write_baton_t *baton = new write_baton_t();
    memset(baton, 0, sizeof(write_baton_t));
    baton->rfcomm = Nan::ObjectWrap::Unwrap<BTSerialPortBindingServer>(info.This());
    baton->rfcomm->Ref();
    baton->buffer.Reset(bufferObject);
    baton->bufferData = bufferData;
    baton->bufferLength = bufferLength;
    baton->callback = new Nan::Callback(info[1].As<Function>());

    queued_write_t *queuedWrite = new queued_write_t();
    memset(queuedWrite, 0, sizeof(queued_write_t));
    queuedWrite->baton = baton;
    queuedWrite->req.data = queuedWrite;

    uv_mutex_lock(&write_queue_mutex);
    bool empty = ngx_queue_empty(&write_queue);

    ngx_queue_insert_tail(&write_queue, &queuedWrite->queue);

    if (empty) {
        uv_queue_work(uv_default_loop(), &queuedWrite->req, EIO_Write, (uv_after_work_cb)EIO_AfterWrite);
    }
    uv_mutex_unlock(&write_queue_mutex);

    return;
}

NAN_METHOD(BTSerialPortBindingServer::Close) {
    BTSerialPortBindingServer* rfcomm = Nan::ObjectWrap::Unwrap<BTSerialPortBindingServer>(info.This());

    if (rfcomm->mClientSocket != 0) {
        close(rfcomm->mClientSocket);
        rfcomm->mClientSocket = 0;
    }

    if (rfcomm->s != 0) {
        close(rfcomm->s);
        int len = write(rfcomm->rep[1], "close", (strlen("close")+1));
        if(len < 0 && errno != EWOULDBLOCK){
            Nan::ThrowError("Cannot write to pipe!");
        }

        rfcomm->s = 0;
    }    

    // closing pipes
    close(rfcomm->rep[0]);
    close(rfcomm->rep[1]);

    mListenBaton->rfcomm->Unref();
    delete mListenBaton;

    return;
}

NAN_METHOD(BTSerialPortBindingServer::Read) {
    const char *usage = "usage: read(callback)";
    if (info.Length() != 1) {
        Nan::ThrowError(usage);
    }

    Local<Function> cb = info[0].As<Function>();

    BTSerialPortBindingServer* rfcomm = Nan::ObjectWrap::Unwrap<BTSerialPortBindingServer>(info.This());

    // callback with an error if the connection has been closed.
    if (rfcomm->mClientSocket == 0) {
        Local<Value> argv[2];

        argv[0] = Nan::Error("The connection has been closed");
        argv[1] = Nan::Undefined();

        Nan::Callback *nc = new Nan::Callback(cb);
        nc->Call(2, argv);
    } else {
        read_baton_t *baton = new read_baton_t();
        baton->rfcomm = rfcomm;
        baton->cb = new Nan::Callback(cb);
        baton->request.data = baton;
        baton->rfcomm->Ref();

        uv_queue_work(uv_default_loop(), &baton->request, EIO_Read, (uv_after_work_cb)EIO_AfterRead);
    }

    return;
}

BTSerialPortBindingServer::ClientWorker::ClientWorker(Nan::Callback * cb, listen_baton_t * baton) : 
    Nan::AsyncWorker(cb),
    mBaton(baton)
{}

BTSerialPortBindingServer::ClientWorker::~ClientWorker()
{}


void BTSerialPortBindingServer::ClientWorker::Execute(){
    if(mBaton == nullptr){
        Nan::ThrowError("listen_baton_t is null!");
    }

    struct sockaddr_rc clientAddress = {
        0x00,
        { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
        0x00
    };
    socklen_t clientAddrLen = sizeof(clientAddress);   
    mBaton->rfcomm->mClientSocket = accept(mBaton->rfcomm->s, (struct sockaddr *)&clientAddress, &clientAddrLen);
    ba2str(&clientAddress.rc_bdaddr, mBaton->clientAddress);
}

void BTSerialPortBindingServer::ClientWorker::HandleOKCallback(){
    Nan::HandleScope scope;

    if(mBaton->rfcomm->mSdpSession){
        // Close the connection with the SDP server so it stops advertising the service
        sdp_close(mBaton->rfcomm->mSdpSession);
        mBaton->rfcomm->mSdpSession = nullptr;
    }

    if(mBaton->rfcomm->mClientSocket == -1){
        mBaton->status = -1;
        sprintf(mBaton->errorString, "accept() failed!. errno: %d", errno);
        Local<Value> argv[] = {
            Nan::Error(mBaton->errorString)
        };
        mBaton->ecb->Call(1,argv);
    }else{
        Local<Value> argv[] = {
            Nan::New<v8::String>((mBaton->clientAddress)).ToLocalChecked()
        };
        callback->Call(1, argv);
    }

    // We can remove the callbacks, but not the entire baton. We will do it later, when the connection is closed.
    mBaton->ecb->Reset();
    mBaton->cb->Reset();
}
