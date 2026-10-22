/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel_async.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include "open62541/common.h"

#include "client/ua_client_internal.h"
#include "server/ua_server_internal.h"

#include <check.h>
#include <stdlib.h>

#include "test_helpers.h"
#include "testing_clock.h"
#include "thread_wrapper.h"

#ifdef __unix__
# include <unistd.h>
#elif defined _WIN32
# include <windows.h>
#define sleep(x) Sleep(1000 * (x))
#endif

UA_Server *server;
UA_Boolean connected = false;

static void
currentState(UA_Client *client, UA_SecureChannelState channelState,
             UA_SessionState sessionState, UA_StatusCode recoveryStatus) {
    if(sessionState == UA_SESSIONSTATE_ACTIVATED)
        connected = true;
    else
        connected = false;
}

static void setup(void) {
    server = UA_Server_newForUnitTest();
    ck_assert(server != NULL);
    UA_Server_run_startup(server);
}

static void teardown(void) {
    UA_Server_run_shutdown(server);
    UA_Server_delete(server);
}

static void
asyncBrowseCallback(UA_Client *Client, void *userdata,
                  UA_UInt32 requestId, UA_BrowseResponse *response) {
    UA_UInt16 *asyncCounter = (UA_UInt16*)userdata;
    (*asyncCounter)++;
}

START_TEST(Client_connect_async) {
    UA_StatusCode retval;
    UA_Client *client = UA_Client_newForUnitTest();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    cc->stateCallback = currentState;
    connected = false;
    UA_Client_connectAsync(client, "opc.tcp://localhost:4840");
    UA_Server_run_iterate(server, false);

    UA_UInt32 reqId = 0;
    UA_UInt16 asyncCounter = 0;
    UA_BrowseRequest bReq;
    UA_BrowseRequest_init (&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new ();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = UA_NODEID_NUMERIC (0, UA_NS0ID_OBJECTSFOLDER);
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL; /* return everything */
    /* Connected gets updated when client is connected */

    while(!connected) {
        UA_Server_run_iterate(server, false);
        retval = UA_Client_run_iterate(client, 0);
        ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);
    }

    /* If not connected requests are not sent */
    UA_Client_sendAsyncBrowseRequest(client, &bReq, asyncBrowseCallback,
                                     &asyncCounter, &reqId);
    UA_Server_run_iterate(server, false);
    retval = UA_Client_run_iterate(client, 0);
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    UA_BrowseRequest_clear(&bReq);
    ck_assert_uint_eq(connected, true);
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);
    UA_Client_disconnectAsync(client);
    while(connected) {
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 0);
    }
    UA_Client_delete (client);
}
END_TEST

UA_SecureChannelState abortState;
static void
abortSecureChannelConnect(UA_Client *client, UA_SecureChannelState channelState,
                          UA_SessionState sessionState, UA_StatusCode recoveryStatus) {
    if(channelState >= abortState)
        UA_Client_disconnectAsync(client);
}

/* Abort the connection by calling disconnect */
START_TEST(Client_connect_async_abort) {
    UA_Client *client = UA_Client_newForUnitTest();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    cc->stateCallback = abortSecureChannelConnect;

    for(int i = 0; i < UA_SECURECHANNELSTATE_CLOSING; i++) {
        abortState = (UA_SecureChannelState)i;
        UA_Client_connectAsync(client, "opc.tcp://localhost:4840");

        UA_SecureChannelState currentState;
        do {
            UA_Server_run_iterate(server, false);
            UA_Client_run_iterate(client, 5);
            UA_Client_getState(client, &currentState, NULL, NULL);
        } while(currentState != UA_SECURECHANNELSTATE_CLOSED);
    }

    UA_Client_delete(client);
}
END_TEST

START_TEST(Client_no_connection) {
    UA_Client *client = UA_Client_newForUnitTest();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    cc->stateCallback = currentState;
    connected = false;
    UA_StatusCode retval = UA_Client_connectAsync(client, "opc.tcp://localhost:4840");
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    /* Wait for the initial socket */
    //UA_EventLoop *el = client->config.eventLoop;
    while(client->channel.state != UA_SECURECHANNELSTATE_OPEN) {
        //el->run(el, 0);
        UA_Server_run_iterate(server, true);
        UA_Client_run_iterate(client, 1);
    }

    /* Manually close the TCP connection */
    UA_ConnectionManager *cm = client->channel.connectionManager;
    cm->closeConnection(cm, client->channel.connectionId);

    do {
        UA_Server_run_iterate(server, false);
        retval |= UA_Client_run_iterate(client, 0);
        ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);
    } while(client->channel.state != UA_SECURECHANNELSTATE_OPEN);

    UA_Client_disconnect(client);
    UA_Client_delete(client);
}
END_TEST

START_TEST(Client_without_run_iterate) {
    UA_Client *client = UA_Client_newForUnitTest();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    cc->stateCallback = currentState;
    connected = false;
    UA_Client_connectAsync(client, "opc.tcp://localhost:4840");
    UA_Client_delete(client);
}
END_TEST

START_TEST(Client_run_iterate) {
    UA_StatusCode retval;
    UA_Client *client = UA_Client_newForUnitTest();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    cc->stateCallback = currentState;
    connected = false;
    retval = UA_Client_connectAsync(client, "opc.tcp://localhost:4840");
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);
    while(!connected) {
        UA_Server_run_iterate(server, false);
        retval = UA_Client_run_iterate(client, 0);
        ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);
    }

    UA_Client_disconnectAsync(client);
    while(client->channel.state != UA_SECURECHANNELSTATE_CLOSED) {
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 0);
    }
    UA_Client_delete(client);
}
END_TEST

START_TEST(Client_connectSecureChannelAsync) {
    /* Connect only the SecureChannel without opening a Session */
    UA_Client *client = UA_Client_newForUnitTest();
    UA_StatusCode retval = UA_Client_connectSecureChannelAsync(client,
                                "opc.tcp://localhost:4840");
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    /* Drive both sides until the channel is open */
    while(client->channel.state != UA_SECURECHANNELSTATE_OPEN) {
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 0);
    }

    UA_SecureChannelState channelState;
    UA_SessionState sessionState;
    UA_StatusCode connectStatus;
    UA_Client_getState(client, &channelState, &sessionState, &connectStatus);
    ck_assert_uint_eq(channelState, UA_SECURECHANNELSTATE_OPEN);
    /* No session should be created */
    ck_assert(sessionState != UA_SESSIONSTATE_ACTIVATED);

    UA_Client_disconnectAsync(client);
    while(client->channel.state != UA_SECURECHANNELSTATE_CLOSED) {
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 0);
    }
    UA_Client_delete(client);
}
END_TEST

START_TEST(Client_connect_async_wrongUrl) {
    /* Attempt to connect to a wrong port — should fail gracefully */
    UA_Client *client = UA_Client_newForUnitTest();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    cc->timeout = 200; /* short timeout */
    UA_StatusCode retval = UA_Client_connectAsync(client, "opc.tcp://localhost:4841");
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    /* Drive a few iterations */
    for(int i = 0; i < 50; i++) {
        UA_fakeSleep(10);
        UA_Client_run_iterate(client, 0);
    }

    UA_SecureChannelState channelState;
    UA_StatusCode connectStatus;
    UA_Client_getState(client, &channelState, NULL, &connectStatus);
    /* Either still connecting or failed — not OPEN */
    ck_assert(channelState != UA_SECURECHANNELSTATE_OPEN ||
              connectStatus != UA_STATUSCODE_GOOD);

    UA_Client_delete(client);
}
END_TEST

START_TEST(Client_connectUsername_async) {
    /* Connect with username/password via the sync connectUsername wrapper,
     * but using manual iteration for the server side */
    UA_Client *client = UA_Client_newForUnitTest();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    cc->stateCallback = currentState;
    connected = false;
    UA_StatusCode retval = UA_Client_connectAsync(client, "opc.tcp://localhost:4840");
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    while(!connected) {
        UA_Server_run_iterate(server, false);
        retval = UA_Client_run_iterate(client, 0);
        ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);
    }

    /* Verify the client is connected with an active session */
    UA_SecureChannelState channelState;
    UA_SessionState sessionState;
    UA_Client_getState(client, &channelState, &sessionState, NULL);
    ck_assert_uint_eq(channelState, UA_SECURECHANNELSTATE_OPEN);
    ck_assert_uint_eq(sessionState, UA_SESSIONSTATE_ACTIVATED);

    /* Disconnect asynchronously */
    UA_Client_disconnectAsync(client);
    while(client->channel.state != UA_SECURECHANNELSTATE_CLOSED) {
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 0);
    }
    UA_Client_delete(client);
}
END_TEST

/* Iterate server and client (in that order) until the client reaches the
 * SessionState target, the session is activated or the connectStatus turns
 * bad. Returns the reached SessionState. */
static UA_SessionState
iterateUntilSessionState(UA_Client *client, UA_SessionState target) {
    UA_SessionState ss = UA_SESSIONSTATE_CLOSED;
    UA_StatusCode cs = UA_STATUSCODE_GOOD;
    for(size_t i = 0; i < 2000; i++) {
        UA_Client_getState(client, NULL, &ss, &cs);
        if(ss == target || ss == UA_SESSIONSTATE_ACTIVATED ||
           cs != UA_STATUSCODE_GOOD)
            break;
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 1);
    }
    return ss;
}

static void
readValueCallback(UA_Client *client, void *userdata, UA_UInt32 requestId,
                  UA_StatusCode status, UA_DataValue *value) {
    UA_StatusCode *res = (UA_StatusCode*)userdata;
    *res = status;
    if(status == UA_STATUSCODE_GOOD && !value->hasValue)
        *res = UA_STATUSCODE_BADUNEXPECTEDERROR;
}

/* The client recovered completely: Session activated, connectStatus good and
 * a service call goes through. */
static void
assertRecovered(UA_Client *client) {
    UA_SessionState ss =
        iterateUntilSessionState(client, UA_SESSIONSTATE_ACTIVATED);
    UA_SecureChannelState cs;
    UA_StatusCode connectStatus;
    UA_Client_getState(client, &cs, &ss, &connectStatus);
    ck_assert_msg(connectStatus == UA_STATUSCODE_GOOD,
                  "The client latched the connectStatus %s and does not recover",
                  UA_StatusCode_name(connectStatus));
    ck_assert_uint_eq(cs, UA_SECURECHANNELSTATE_OPEN);
    ck_assert_uint_eq(ss, UA_SESSIONSTATE_ACTIVATED);

    UA_StatusCode readRes = UA_STATUSCODE_BADWAITINGFORRESPONSE;
    UA_StatusCode retval =
        UA_Client_readValueAttribute_async(client,
            UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_STATE),
            readValueCallback, &readRes, NULL);
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);
    for(size_t i = 0; i < 1000 && readRes == UA_STATUSCODE_BADWAITINGFORRESPONSE; i++) {
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 1);
    }
    ck_assert_uint_eq(readRes, UA_STATUSCODE_GOOD);
}

/* Close the Session and the SecureChannel while the server keeps running */
static void
disconnectAndDelete(UA_Client *client) {
    UA_Client_disconnectAsync(client);
    for(size_t i = 0; i < 1000 &&
            client->channel.state != UA_SECURECHANNELSTATE_CLOSED; i++) {
        UA_Server_run_iterate(server, false);
        UA_Client_run_iterate(client, 1);
    }
    UA_Client_delete(client);
}

/* The TCP connection drops while the CreateSession / ActivateSession request
 * is outstanding. The pending request is cancelled locally with
 * BadSecureChannelClosed. That is a transport error and not a rejection of the
 * Session by the server. The client must reconnect on its own and end up with
 * an activated Session. */
static void
dropConnectionDuringSessionHandshake(UA_SessionState dropState) {
    UA_Client *client = UA_Client_newForUnitTest();
    UA_StatusCode retval =
        UA_Client_connectAsync(client, "opc.tcp://localhost:4840");
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    /* The request is sent at the end of the client iteration. The server has
     * not been iterated since, so no response can have been processed. */
    UA_SessionState ss = iterateUntilSessionState(client, dropState);
    ck_assert_uint_eq(ss, dropState);

    /* Drop the TCP connection from below the SecureChannel */
    UA_ConnectionManager *cm = client->channel.connectionManager;
    cm->closeConnection(cm, client->channel.connectionId);

    assertRecovered(client);
    disconnectAndDelete(client);
}

START_TEST(Client_connect_async_drop_during_createSession) {
    dropConnectionDuringSessionHandshake(UA_SESSIONSTATE_CREATE_REQUESTED);
} END_TEST

START_TEST(Client_connect_async_drop_during_activateSession) {
    dropConnectionDuringSessionHandshake(UA_SESSIONSTATE_ACTIVATE_REQUESTED);
} END_TEST

/* A service request is sent while the Session handshake is ongoing. The
 * request carries no AuthenticationToken yet and the server answers it with
 * BadSessionIdInvalid. That response must not tear down the Session that is
 * being created/activated right now. Doing so cancels the outstanding
 * CreateSession/ActivateSession and sends a new CreateSession, orphaning the
 * first Session on the server (and with Sign/SignAndEncrypt the reply to the
 * first CreateSession is checked against the nonce of the second one). */
START_TEST(Client_connect_async_service_during_createSession) {
    UA_Client *client = UA_Client_newForUnitTest();
    UA_StatusCode retval =
        UA_Client_connectAsync(client, "opc.tcp://localhost:4840");
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    UA_SessionState ss =
        iterateUntilSessionState(client, UA_SESSIONSTATE_CREATE_REQUESTED);
    ck_assert_uint_eq(ss, UA_SESSIONSTATE_CREATE_REQUESTED);

    /* Send a Read before the Session is activated */
    UA_StatusCode readRes = UA_STATUSCODE_BADWAITINGFORRESPONSE;
    retval = UA_Client_readValueAttribute_async(client,
                 UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERSTATUS_STATE),
                 readValueCallback, &readRes, NULL);
    ck_assert_uint_eq(retval, UA_STATUSCODE_GOOD);

    assertRecovered(client);
    ck_assert_uint_ne(readRes, UA_STATUSCODE_GOOD); /* Rejected by the server */

    /* Only the one Session was created. server->sessionCount counts created
     * Sessions; the public statistics count activated ones only and cannot
     * see the orphaned Session, which is never activated. */
    ck_assert_uint_eq(server->sessionCount, 1);

    disconnectAndDelete(client);
} END_TEST

static Suite* testSuite_Client(void) {
    Suite *s = suite_create("Client");
    TCase *tc_client_connect = tcase_create("Client Connect Async");
    tcase_add_checked_fixture(tc_client_connect, setup, teardown);
    tcase_add_test(tc_client_connect, Client_connect_async);
    tcase_add_test(tc_client_connect, Client_connect_async_abort);
    tcase_add_test(tc_client_connect, Client_no_connection);
    tcase_add_test(tc_client_connect, Client_without_run_iterate);
    tcase_add_test(tc_client_connect, Client_run_iterate);
    tcase_add_test(tc_client_connect, Client_connectSecureChannelAsync);
    tcase_add_test(tc_client_connect, Client_connect_async_wrongUrl);
    tcase_add_test(tc_client_connect, Client_connectUsername_async);
    tcase_add_test(tc_client_connect, Client_connect_async_drop_during_createSession);
    tcase_add_test(tc_client_connect, Client_connect_async_drop_during_activateSession);
    tcase_add_test(tc_client_connect, Client_connect_async_service_during_createSession);
    suite_add_tcase(s,tc_client_connect);
    return s;
}

int main(void) {
    Suite *s = testSuite_Client();
    SRunner *sr = srunner_create(s);
    srunner_set_fork_status(sr, CK_NOFORK);
    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
